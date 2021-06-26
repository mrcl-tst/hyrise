#include "dips_pruning_rule.hpp"
#include <iostream>
#include "expression/abstract_expression.hpp"
#include "expression/binary_predicate_expression.hpp"
#include "expression/expression_functional.hpp"
#include "hyrise.hpp"
#include "logical_query_plan/abstract_lqp_node.hpp"
#include "logical_query_plan/join_node.hpp"
#include "logical_query_plan/logical_plan_root_node.hpp"
#include "logical_query_plan/lqp_utils.hpp"
#include "logical_query_plan/predicate_node.hpp"
#include "logical_query_plan/stored_table_node.hpp"
#include "optimizer/strategy/chunk_pruning_rule.hpp"
#include "resolve_type.hpp"
#include "statistics/attribute_statistics.hpp"
#include "statistics/base_attribute_statistics.hpp"
#include "statistics/statistics_objects/min_max_filter.hpp"
#include "statistics/statistics_objects/range_filter.hpp"
#include "statistics/table_statistics.hpp"

// #include "hyrise.hpp"
// #include <fstream>

namespace opossum {

void DipsPruningRule::_bottom_up_dip_traversal(
    const std::shared_ptr<DipsJoinGraphNode>& node) const {  // expects root in the first call
  for (const auto& child : node->children) {
    _bottom_up_dip_traversal(child);
  }
  if (node->parent == nullptr) {  // handle root
    return;
  }
  auto edge = node->get_edge_for_table(node->parent);

  for (const auto& predicate : edge->predicates) {
    auto left_operand = predicate->left_operand();
    auto right_operand = predicate->right_operand();

    auto left_lqp = std::dynamic_pointer_cast<LQPColumnExpression>(left_operand);
    auto right_lqp = std::dynamic_pointer_cast<LQPColumnExpression>(right_operand);

    Assert(left_lqp && right_lqp, "Expected LQPColumnExpression!");

    auto l = std::dynamic_pointer_cast<const StoredTableNode>(left_lqp->original_node.lock());
    auto r = std::dynamic_pointer_cast<const StoredTableNode>(right_lqp->original_node.lock());

    Assert(l && r, "Expected StoredTableNode");

    std::shared_ptr<StoredTableNode> left_stored_table_node = std::const_pointer_cast<StoredTableNode>(l);
    std::shared_ptr<StoredTableNode> right_stored_table_node = std::const_pointer_cast<StoredTableNode>(r);

    if (!left_stored_table_node || !right_stored_table_node) {
      return;
    }

    // LEFT -> RIGHT
    _dips_pruning(left_stored_table_node, left_lqp->original_column_id, right_stored_table_node,
                  right_lqp->original_column_id);

    // RIGHT -> LEFT
    _dips_pruning(right_stored_table_node, right_lqp->original_column_id, left_stored_table_node,
                  left_lqp->original_column_id);
  }
}

void DipsPruningRule::_top_down_dip_traversal(
    const std::shared_ptr<DipsJoinGraphNode>& node) const {  // expects root in the first call
  if (node->parent != nullptr) {                             // handle root
    auto edge = node->get_edge_for_table(node->parent);

    for (const auto& predicate : edge->predicates) {
      auto left_operand = predicate->left_operand();
      auto right_operand = predicate->right_operand();

      auto left_lqp = std::dynamic_pointer_cast<LQPColumnExpression>(left_operand);
      auto right_lqp = std::dynamic_pointer_cast<LQPColumnExpression>(right_operand);

      Assert(left_lqp && right_lqp, "Expected LQPColumnExpression!");

      auto l = std::dynamic_pointer_cast<const StoredTableNode>(left_lqp->original_node.lock());
      auto r = std::dynamic_pointer_cast<const StoredTableNode>(right_lqp->original_node.lock());

      Assert(l && r, "Expected StoredTableNode");

      std::shared_ptr<StoredTableNode> left_stored_table_node = std::const_pointer_cast<StoredTableNode>(l);
      std::shared_ptr<StoredTableNode> right_stored_table_node = std::const_pointer_cast<StoredTableNode>(r);

      if (!left_stored_table_node || !right_stored_table_node) {
        return;
      }

      // LEFT -> RIGHT
      _dips_pruning(left_stored_table_node, left_lqp->original_column_id, right_stored_table_node,
                    right_lqp->original_column_id);

      // RIGHT -> LEFT
      _dips_pruning(right_stored_table_node, right_lqp->original_column_id, left_stored_table_node,
                    left_lqp->original_column_id);
    }
  }

  for (const auto& child : node->children) {
    _top_down_dip_traversal(child);
  }
}

// To be able to push dips through joins we first need to construct a graph on which we can execute the main algorithm.
//  We are doing this by recursively traversing over the LQP graph. In every visit of a node the following steps are
// executed:
// 1. Check that the currently visited node is a join node.
// 2. Get the join predicates
// 3. Check that the left and right operands are LQPColumnExpression.
// 4. Get each of the associated StoredTableNode of the left and right expressions.
// 5. Add both of the storage nodes to the graph (if they are not in it) and connect them with edges (if they are not
// connected).
// 6. Add the predicates to the associated edges.
void DipsPruningRule::_build_join_graph(const std::shared_ptr<AbstractLQPNode>& node,
                                        const std::shared_ptr<DipsJoinGraph>& join_graph) const {
  // Why do we exit in this cases ?
  if (node->type == LQPNodeType::Union || node->type == LQPNodeType::Intersect || node->type == LQPNodeType::Except) {
    return;
  }

  if (node->left_input()) _build_join_graph(node->left_input(), join_graph);
  if (node->right_input()) _build_join_graph(node->right_input(), join_graph);

  // This rule only supports the inner and semi join
  if (node->type == LQPNodeType::Join) {
    if (std::find(supported_join_types.begin(), supported_join_types.end(),
                  std::dynamic_pointer_cast<JoinNode>(node)->join_mode) == supported_join_types.end()) {
      return;
    }
    const auto& join_node = static_cast<JoinNode&>(*node);
    const auto& join_predicates = join_node.join_predicates();

    for (const auto& predicate : join_predicates) {
      // Why do we need to cast the predicates to binary predicate expressions?
      std::shared_ptr<BinaryPredicateExpression> binary_predicate =
          std::dynamic_pointer_cast<BinaryPredicateExpression>(predicate);

      Assert(binary_predicate, "Expected BinaryPredicateExpression!");

      // We are only interested in equal predicate conditions (The dibs rule is only working with equal predicates)
      if (binary_predicate->predicate_condition != PredicateCondition::Equals) {
        continue;
      }

      auto left_operand = binary_predicate->left_operand();
      auto right_operand = binary_predicate->right_operand();

      auto left_lqp = std::dynamic_pointer_cast<LQPColumnExpression>(left_operand);
      auto right_lqp = std::dynamic_pointer_cast<LQPColumnExpression>(right_operand);

      // We need to check that the type is LQPColumn
      if (!left_lqp || !right_lqp) {
        continue;
      }

      auto l = std::dynamic_pointer_cast<const StoredTableNode>(left_lqp->original_node.lock());
      auto r = std::dynamic_pointer_cast<const StoredTableNode>(right_lqp->original_node.lock());

      Assert(l && r, "Expected StoredTableNode");

      std::shared_ptr<StoredTableNode> left_stored_table_node = std::const_pointer_cast<StoredTableNode>(l);
      std::shared_ptr<StoredTableNode> right_stored_table_node = std::const_pointer_cast<StoredTableNode>(r);

      // access join graph nodes (every storage table note is represented inside the join graph)
      auto left_join_graph_node = join_graph->get_node_for_table(left_stored_table_node);
      auto right_join_graph_node = join_graph->get_node_for_table(right_stored_table_node);

      // access edges (Connect left and right node with edges if there are none)
      auto left_right_edge = left_join_graph_node->get_edge_for_table(right_join_graph_node);
      auto right_left_edge = right_join_graph_node->get_edge_for_table(left_join_graph_node);

      // append predicates
      left_right_edge->append_predicate(
          binary_predicate);  // TODO(somebody): visit every node in LQP only once (avoid cycles) -> use "simple" append
      right_left_edge->append_predicate(binary_predicate);
    }
  }
}

void DipsPruningRule::_extend_pruned_chunks(const std::shared_ptr<StoredTableNode>& table_node,
                                            const std::set<ChunkID>& pruned_chunk_ids) {
  const auto& already_pruned_chunk_ids = table_node->pruned_chunk_ids();

  if (!already_pruned_chunk_ids.empty()) {
    std::vector<ChunkID> union_values;
    std::set_union(already_pruned_chunk_ids.begin(), already_pruned_chunk_ids.end(), pruned_chunk_ids.begin(),
                   pruned_chunk_ids.end(), std::back_inserter(union_values));
    table_node->set_pruned_chunk_ids(union_values);
  } else {
    table_node->set_pruned_chunk_ids(std::vector<ChunkID>(pruned_chunk_ids.begin(), pruned_chunk_ids.end()));
  }
}

void DipsPruningRule::_dips_pruning(const std::shared_ptr<const StoredTableNode> table_node, ColumnID column_id,
                                    std::shared_ptr<StoredTableNode> join_partner_table_node,
                                    ColumnID join_partner_column_id) {
  auto table = Hyrise::get().storage_manager.get_table(table_node->table_name);

  resolve_data_type(table->column_data_type(column_id), [&](const auto data_type_t) {
    using ColumnDataType = typename decltype(data_type_t)::type;

    // TODO(somebody): check if pointers would be more efficient
    auto base_ranges = _get_not_pruned_range_statistics<ColumnDataType>(table_node, column_id);
    auto partner_ranges =
        _get_not_pruned_range_statistics<ColumnDataType>(join_partner_table_node, join_partner_column_id);
    auto pruned_chunks = _calculate_pruned_chunks<ColumnDataType>(base_ranges, partner_ranges);
    _extend_pruned_chunks(join_partner_table_node, pruned_chunks);
  });
}

std::ostream& operator<<(std::ostream& stream, const DipsJoinGraph& join_graph) {
  stream << "==== Vertices ====" << std::endl;
  if (join_graph.nodes.empty()) {
    stream << "<none>" << std::endl;
  } else {
    for (const auto& node : join_graph.nodes) {
      stream << node->table_node->description() << std::endl;
      stream << "      ==== Adress ====" << std::endl;
      stream << "          " << node << std::endl;
      stream << "      ==== Parent ====" << std::endl;
      stream << "          " << node->parent << std::endl;
      stream << "      ==== Children ====" << std::endl;
      for (const auto& child : node->children) {
        stream << "          " << child << std::endl;
      }

      stream << "      ==== Edges ====" << std::endl;
      for (const auto& edge : node->edges) {
        stream << "      " << edge->partner_node->table_node->description() << std::endl;
        stream << "            ==== Predicates ====" << std::endl;
        for (const auto& predicate : edge->predicates) {
          stream << "            " << predicate->description(AbstractExpression::DescriptionMode::ColumnName)
                 << std::endl;
        }
      }
    }
  }

  return stream;
}

void DipsPruningRule::_apply_to_plan_without_subqueries(const std::shared_ptr<AbstractLQPNode>& lqp_root) const {
  std::shared_ptr<DipsJoinGraph> join_graph = std::make_shared<DipsJoinGraph>();
  _build_join_graph(lqp_root, join_graph);

  if (join_graph->is_empty()) {
    return;
  }

  if (join_graph->is_tree()) {
    std::shared_ptr<DipsJoinGraphNode> root =
        join_graph
            ->nodes[0];  // Note: we don't use parallel JoinGraph traversal, thus root node can be chosen arbitrary
    join_graph->set_root(root);
    _bottom_up_dip_traversal(root);
    _top_down_dip_traversal(root);
  } else {
    // Assumption: Hyrise handles cycles itself
  }
}

}  // namespace opossum
