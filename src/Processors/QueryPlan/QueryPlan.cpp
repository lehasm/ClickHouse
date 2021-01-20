#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/IQueryPlanStep.h>
#include <Processors/QueryPipeline.h>
#include <IO/WriteBuffer.h>
#include <IO/Operators.h>
#include <Interpreters/ActionsDAG.h>
#include <Interpreters/ArrayJoinAction.h>
#include <stack>
#include <Processors/QueryPlan/LimitStep.h>
#include "MergingSortedStep.h"
#include "FinishSortingStep.h"
#include "MergeSortingStep.h"
#include "PartialSortingStep.h"
#include "TotalsHavingStep.h"
#include "ExpressionStep.h"
#include "ArrayJoinStep.h"
#include "FilterStep.h"

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

QueryPlan::QueryPlan() = default;
QueryPlan::~QueryPlan() = default;
QueryPlan::QueryPlan(QueryPlan &&) = default;
QueryPlan & QueryPlan::operator=(QueryPlan &&) = default;

void QueryPlan::checkInitialized() const
{
    if (!isInitialized())
        throw Exception("QueryPlan was not initialized", ErrorCodes::LOGICAL_ERROR);
}

void QueryPlan::checkNotCompleted() const
{
    if (isCompleted())
        throw Exception("QueryPlan was already completed", ErrorCodes::LOGICAL_ERROR);
}

bool QueryPlan::isCompleted() const
{
    return isInitialized() && !root->step->hasOutputStream();
}

const DataStream & QueryPlan::getCurrentDataStream() const
{
    checkInitialized();
    checkNotCompleted();
    return root->step->getOutputStream();
}

void QueryPlan::unitePlans(QueryPlanStepPtr step, std::vector<std::unique_ptr<QueryPlan>> plans)
{
    if (isInitialized())
        throw Exception("Cannot unite plans because current QueryPlan is already initialized",
                        ErrorCodes::LOGICAL_ERROR);

    const auto & inputs = step->getInputStreams();
    size_t num_inputs = step->getInputStreams().size();
    if (num_inputs != plans.size())
    {
        throw Exception("Cannot unite QueryPlans using " + step->getName() +
                        " because step has different number of inputs. "
                        "Has " + std::to_string(plans.size()) + " plans "
                        "and " + std::to_string(num_inputs) + " inputs", ErrorCodes::LOGICAL_ERROR);
    }

    for (size_t i = 0; i < num_inputs; ++i)
    {
        const auto & step_header = inputs[i].header;
        const auto & plan_header = plans[i]->getCurrentDataStream().header;
        if (!blocksHaveEqualStructure(step_header, plan_header))
            throw Exception("Cannot unite QueryPlans using " + step->getName() + " because "
                            "it has incompatible header with plan " + root->step->getName() + " "
                            "plan header: " + plan_header.dumpStructure() +
                            "step header: " + step_header.dumpStructure(), ErrorCodes::LOGICAL_ERROR);
    }

    for (auto & plan : plans)
        nodes.splice(nodes.end(), std::move(plan->nodes));

    nodes.emplace_back(Node{.step = std::move(step)});
    root = &nodes.back();

    for (auto & plan : plans)
        root->children.emplace_back(plan->root);

    for (auto & plan : plans)
    {
        max_threads = std::max(max_threads, plan->max_threads);
        interpreter_context.insert(interpreter_context.end(),
                                   plan->interpreter_context.begin(), plan->interpreter_context.end());
    }
}

void QueryPlan::addStep(QueryPlanStepPtr step)
{
    checkNotCompleted();

    size_t num_input_streams = step->getInputStreams().size();

    if (num_input_streams == 0)
    {
        if (isInitialized())
            throw Exception("Cannot add step " + step->getName() + " to QueryPlan because "
                            "step has no inputs, but QueryPlan is already initialized", ErrorCodes::LOGICAL_ERROR);

        nodes.emplace_back(Node{.step = std::move(step)});
        root = &nodes.back();
        return;
    }

    if (num_input_streams == 1)
    {
        if (!isInitialized())
            throw Exception("Cannot add step " + step->getName() + " to QueryPlan because "
                            "step has input, but QueryPlan is not initialized", ErrorCodes::LOGICAL_ERROR);

        const auto & root_header = root->step->getOutputStream().header;
        const auto & step_header = step->getInputStreams().front().header;
        if (!blocksHaveEqualStructure(root_header, step_header))
            throw Exception("Cannot add step " + step->getName() + " to QueryPlan because "
                            "it has incompatible header with root step " + root->step->getName() + " "
                            "root header: " + root_header.dumpStructure() +
                            "step header: " + step_header.dumpStructure(), ErrorCodes::LOGICAL_ERROR);

        nodes.emplace_back(Node{.step = std::move(step), .children = {root}});
        root = &nodes.back();
        return;
    }

    throw Exception("Cannot add step " + step->getName() + " to QueryPlan because it has " +
                    std::to_string(num_input_streams) + " inputs but " + std::to_string(isInitialized() ? 1 : 0) +
                    " input expected", ErrorCodes::LOGICAL_ERROR);
}

QueryPipelinePtr QueryPlan::buildQueryPipeline()
{
    checkInitialized();
    optimize();

    struct Frame
    {
        Node * node;
        QueryPipelines pipelines = {};
    };

    QueryPipelinePtr last_pipeline;

    std::stack<Frame> stack;
    stack.push(Frame{.node = root});

    while (!stack.empty())
    {
        auto & frame = stack.top();

        if (last_pipeline)
        {
            frame.pipelines.emplace_back(std::move(last_pipeline));
            last_pipeline = nullptr;
        }

        size_t next_child = frame.pipelines.size();
        if (next_child == frame.node->children.size())
        {
            bool limit_max_threads = frame.pipelines.empty();
            last_pipeline = frame.node->step->updatePipeline(std::move(frame.pipelines));

            if (limit_max_threads && max_threads)
                last_pipeline->limitMaxThreads(max_threads);

            stack.pop();
        }
        else
            stack.push(Frame{.node = frame.node->children[next_child]});
    }

    for (auto & context : interpreter_context)
        last_pipeline->addInterpreterContext(std::move(context));

    return last_pipeline;
}

Pipe QueryPlan::convertToPipe()
{
    if (!isInitialized())
        return {};

    if (isCompleted())
        throw Exception("Cannot convert completed QueryPlan to Pipe", ErrorCodes::LOGICAL_ERROR);

    return QueryPipeline::getPipe(std::move(*buildQueryPipeline()));
}

void QueryPlan::addInterpreterContext(std::shared_ptr<Context> context)
{
    interpreter_context.emplace_back(std::move(context));
}


static void explainStep(
    const IQueryPlanStep & step,
    IQueryPlanStep::FormatSettings & settings,
    const QueryPlan::ExplainPlanOptions & options)
{
    std::string prefix(settings.offset, ' ');
    settings.out << prefix;
    settings.out << step.getName();

    const auto & description = step.getStepDescription();
    if (options.description && !description.empty())
        settings.out <<" (" << description << ')';

    settings.out.write('\n');

    if (options.header)
    {
        settings.out << prefix;

        if (!step.hasOutputStream())
            settings.out << "No header";
        else if (!step.getOutputStream().header)
            settings.out << "Empty header";
        else
        {
            settings.out << "Header: ";
            bool first = true;

            for (const auto & elem : step.getOutputStream().header)
            {
                if (!first)
                    settings.out << "\n" << prefix << "        ";

                first = false;
                elem.dumpNameAndType(settings.out);
            }
        }

        settings.out.write('\n');
    }

    if (options.actions)
        step.describeActions(settings);
}

std::string debugExplainStep(const IQueryPlanStep & step)
{
    WriteBufferFromOwnString out;
    IQueryPlanStep::FormatSettings settings{.out = out};
    QueryPlan::ExplainPlanOptions options{.actions = true};
    explainStep(step, settings, options);
    return out.str();
}

void QueryPlan::explainPlan(WriteBuffer & buffer, const ExplainPlanOptions & options)
{
    checkInitialized();

    IQueryPlanStep::FormatSettings settings{.out = buffer, .write_header = options.header};

    struct Frame
    {
        Node * node;
        bool is_description_printed = false;
        size_t next_child = 0;
    };

    std::stack<Frame> stack;
    stack.push(Frame{.node = root});

    while (!stack.empty())
    {
        auto & frame = stack.top();

        if (!frame.is_description_printed)
        {
            settings.offset = (stack.size() - 1) * settings.indent;
            explainStep(*frame.node->step, settings, options);
            frame.is_description_printed = true;
        }

        if (frame.next_child < frame.node->children.size())
        {
            stack.push(Frame{frame.node->children[frame.next_child]});
            ++frame.next_child;
        }
        else
            stack.pop();
    }
}

static void explainPipelineStep(IQueryPlanStep & step, IQueryPlanStep::FormatSettings & settings)
{
    settings.out << String(settings.offset, settings.indent_char) << "(" << step.getName() << ")\n";
    size_t current_offset = settings.offset;
    step.describePipeline(settings);
    if (current_offset == settings.offset)
        settings.offset += settings.indent;
}

void QueryPlan::explainPipeline(WriteBuffer & buffer, const ExplainPipelineOptions & options)
{
    checkInitialized();

    IQueryPlanStep::FormatSettings settings{.out = buffer, .write_header = options.header};

    struct Frame
    {
        Node * node;
        size_t offset = 0;
        bool is_description_printed = false;
        size_t next_child = 0;
    };

    std::stack<Frame> stack;
    stack.push(Frame{.node = root});

    while (!stack.empty())
    {
        auto & frame = stack.top();

        if (!frame.is_description_printed)
        {
            settings.offset = frame.offset;
            explainPipelineStep(*frame.node->step, settings);
            frame.offset = settings.offset;
            frame.is_description_printed = true;
        }

        if (frame.next_child < frame.node->children.size())
        {
            stack.push(Frame{frame.node->children[frame.next_child], frame.offset});
            ++frame.next_child;
        }
        else
            stack.pop();
    }
}

/// If plan looks like Limit -> Sorting, update limit for Sorting
bool tryUpdateLimitForSortingSteps(QueryPlan::Node * node, size_t limit)
{
    if (limit == 0)
        return false;

    QueryPlanStepPtr & step = node->step;
    QueryPlan::Node * child = nullptr;
    bool updated = false;

    if (auto * merging_sorted = typeid_cast<MergingSortedStep *>(step.get()))
    {
        /// TODO: remove LimitStep here.
        merging_sorted->updateLimit(limit);
        updated = true;
        child = node->children.front();
    }
    else if (auto * finish_sorting = typeid_cast<FinishSortingStep *>(step.get()))
    {
        /// TODO: remove LimitStep here.
        finish_sorting->updateLimit(limit);
        updated = true;
    }
    else if (auto * merge_sorting = typeid_cast<MergeSortingStep *>(step.get()))
    {
        merge_sorting->updateLimit(limit);
        updated = true;
        child = node->children.front();
    }
    else if (auto * partial_sorting = typeid_cast<PartialSortingStep *>(step.get()))
    {
        partial_sorting->updateLimit(limit);
        updated = true;
    }

    /// We often have chain PartialSorting -> MergeSorting -> MergingSorted
    /// Try update limit for them also if possible.
    if (child)
        tryUpdateLimitForSortingSteps(child, limit);

    return updated;
}

/// Move LimitStep down if possible.
static void tryPushDownLimit(QueryPlanStepPtr & parent, QueryPlan::Node * child_node)
{
    auto & child = child_node->step;
    auto * limit = typeid_cast<LimitStep *>(parent.get());

    if (!limit)
        return;

    /// Skip LIMIT WITH TIES by now.
    if (limit->withTies())
        return;

    const auto * transforming = dynamic_cast<const ITransformingStep *>(child.get());

    /// Skip everything which is not transform.
    if (!transforming)
        return;

    /// Special cases for sorting steps.
    if (tryUpdateLimitForSortingSteps(child_node, limit->getLimitForSorting()))
        return;

    /// Special case for TotalsHaving. Totals may be incorrect if we push down limit.
    if (typeid_cast<const TotalsHavingStep *>(child.get()))
        return;

    /// Now we should decide if pushing down limit possible for this step.

    const auto & transform_traits = transforming->getTransformTraits();
    const auto & data_stream_traits = transforming->getDataStreamTraits();

    /// Cannot push down if child changes the number of rows.
    if (!transform_traits.preserves_number_of_rows)
        return;

    /// Cannot push down if data was sorted exactly by child stream.
    if (!child->getOutputStream().sort_description.empty() && !data_stream_traits.preserves_sorting)
        return;

    /// Now we push down limit only if it doesn't change any stream properties.
    /// TODO: some of them may be changed and, probably, not important for following streams. We may add such info.
    if (!limit->getOutputStream().hasEqualPropertiesWith(transforming->getOutputStream()))
        return;

    /// Input stream for Limit have changed.
    limit->updateInputStream(transforming->getInputStreams().front());

    parent.swap(child);
}

/// Move ARRAY JOIN up if possible.
static void tryLiftUpArrayJoin(QueryPlan::Node * parent_node, QueryPlan::Node * child_node, QueryPlan::Nodes & nodes)
{
    auto & parent = parent_node->step;
    auto & child = child_node->step;
    auto * expression_step = typeid_cast<ExpressionStep *>(parent.get());
    auto * filter_step = typeid_cast<FilterStep *>(parent.get());
    auto * array_join_step = typeid_cast<ArrayJoinStep *>(child.get());

    if (!(expression_step || filter_step) || !array_join_step)
        return;

    const auto & array_join = array_join_step->arrayJoin();
    const auto & expression = expression_step ? expression_step->getExpression()
                                              : filter_step->getExpression();

    auto split_actions = expression->splitActionsBeforeArrayJoin(array_join->columns);

    /// No actions can be moved before ARRAY JOIN.
    if (split_actions.first->empty())
        return;

    auto description = parent->getStepDescription();

    /// All actions was moved before ARRAY JOIN. Swap Expression and ArrayJoin.
    if (split_actions.second->empty())
    {
        auto expected_header = parent->getOutputStream().header;

        /// Expression/Filter -> ArrayJoin
        std::swap(parent, child);
        /// ArrayJoin -> Expression/Filter

        if (expression_step)
            child = std::make_unique<ExpressionStep>(child_node->children.at(0)->step->getOutputStream(),
                                                     std::move(split_actions.first));
        else
            child = std::make_unique<FilterStep>(child_node->children.at(0)->step->getOutputStream(),
                                                 std::move(split_actions.first),
                                                 filter_step->getFilterColumnName(),
                                                 filter_step->removesFilterColumn());

        child->setStepDescription(std::move(description));

        array_join_step->updateInputStream(child->getOutputStream(), expected_header);
        return;
    }

    /// Add new expression step before ARRAY JOIN.
    /// Expression/Filter -> ArrayJoin -> Something
    auto & node = nodes.emplace_back();
    node.children.swap(child_node->children);
    child_node->children.emplace_back(&node);
    /// Expression/Filter -> ArrayJoin -> node -> Something

    node.step = std::make_unique<ExpressionStep>(node.children.at(0)->step->getOutputStream(),
                                                 std::move(split_actions.first));
    node.step->setStepDescription(description);
    array_join_step->updateInputStream(node.step->getOutputStream(), {});

    if (expression_step)
        parent = std::make_unique<ExpressionStep>(array_join_step->getOutputStream(), split_actions.second);
    else
        parent = std::make_unique<FilterStep>(array_join_step->getOutputStream(), split_actions.second,
                                              filter_step->getFilterColumnName(), filter_step->removesFilterColumn());

    parent->setStepDescription(description + " [split]");
}

/// Replace chain `ExpressionStep -> ExpressionStep` to single ExpressionStep
/// Replace chain `FilterStep -> ExpressionStep` to single FilterStep
static bool tryMergeExpressions(QueryPlan::Node * parent_node, QueryPlan::Node * child_node)
{
    auto & parent = parent_node->step;
    auto & child = child_node->step;

    auto * parent_expr = typeid_cast<ExpressionStep *>(parent.get());
    auto * parent_filter = typeid_cast<FilterStep *>(parent.get());
    auto * child_expr = typeid_cast<ExpressionStep *>(child.get());

    if (parent_expr && child_expr)
    {
        const auto & child_actions = child_expr->getExpression();
        const auto & parent_actions = parent_expr->getExpression();

        /// We cannot combine actions with arrayJoin and stateful function because we not always can reorder them.
        /// Example: select rowNumberInBlock() from (select arrayJoin([1, 2]))
        /// Such a query will return two zeroes if we combine actions together.
        if (child_actions->hasArrayJoin() && parent_actions->hasStatefulFunctions())
            return false;

        auto merged = ActionsDAG::merge(std::move(*child_actions), std::move(*parent_actions));

        auto expr = std::make_unique<ExpressionStep>(child_expr->getInputStreams().front(), merged);
        expr->setStepDescription("(" + parent_expr->getStepDescription() + " + " + child_expr->getStepDescription() + ")");

        parent_node->step = std::move(expr);
        parent_node->children.swap(child_node->children);
        return true;
    }
    else if (parent_filter && child_expr)
    {
        const auto & child_actions = child_expr->getExpression();
        const auto & parent_actions = parent_filter->getExpression();

        if (child_actions->hasArrayJoin() && parent_actions->hasStatefulFunctions())
            return false;

        auto merged = ActionsDAG::merge(std::move(*child_actions), std::move(*parent_actions));

        auto filter = std::make_unique<FilterStep>(child_expr->getInputStreams().front(), merged,
                                                   parent_filter->getFilterColumnName(), parent_filter->removesFilterColumn());
        filter->setStepDescription("(" + parent_filter->getStepDescription() + " + " + child_expr->getStepDescription() + ")");

        parent_node->step = std::move(filter);
        parent_node->children.swap(child_node->children);
        return true;
    }

    return false;
}

/// Split FilterStep into chain `ExpressionStep -> FilterStep`, where FilterStep contains minimal number of nodes.
static bool trySplitFilter(QueryPlan::Node * node, QueryPlan::Nodes & nodes)
{
    auto * filter_step = typeid_cast<FilterStep *>(node->step.get());
    if (!filter_step)
        return false;

    const auto & expr = filter_step->getExpression();

    /// Do not split if there are function like runningDifference.
    if (expr->hasStatefulFunctions())
        return false;

    auto split = expr->splitActionsForFilter(filter_step->getFilterColumnName());

    if (split.second->empty())
        return false;

    if (filter_step->removesFilterColumn())
        split.second->removeUnusedInput(filter_step->getFilterColumnName());

    auto description = filter_step->getStepDescription();

    auto & filter_node = nodes.emplace_back();
    node->children.swap(filter_node.children);
    node->children.push_back(&filter_node);

    filter_node.step = std::make_unique<FilterStep>(
            filter_node.children.at(0)->step->getOutputStream(),
            std::move(split.first),
            filter_step->getFilterColumnName(),
            filter_step->removesFilterColumn());

    node->step = std::make_unique<ExpressionStep>(filter_node.step->getOutputStream(), std::move(split.second));

    filter_node.step->setStepDescription("(" + description + ")[split]");
    node->step->setStepDescription(description);

    return true;
}

void QueryPlan::optimize()
{
    struct Frame
    {
        Node * node;
        size_t next_child = 0;
    };

    std::stack<Frame> stack;
    stack.push(Frame{.node = root});

    while (!stack.empty())
    {
        auto & frame = stack.top();

        if (frame.next_child == 0)
        {
            if (frame.node->children.size() == 1)
            {
                tryPushDownLimit(frame.node->step, frame.node->children.front());

                while (tryMergeExpressions(frame.node, frame.node->children.front()));

                if (frame.node->children.size() == 1)
                    tryLiftUpArrayJoin(frame.node, frame.node->children.front(), nodes);

                trySplitFilter(frame.node, nodes);
            }
        }

        if (frame.next_child < frame.node->children.size())
        {
            stack.push(Frame{frame.node->children[frame.next_child]});
            ++frame.next_child;
        }
        else
        {
            if (frame.node->children.size() == 1)
            {
                while (tryMergeExpressions(frame.node, frame.node->children.front()));

                trySplitFilter(frame.node, nodes);

                tryLiftUpArrayJoin(frame.node, frame.node->children.front(), nodes);
            }

            stack.pop();
        }
    }
}

}
