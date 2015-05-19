#include <stdio.h>
#include <stdbool.h>
#include "tree_sitter/runtime.h"
#include "tree_sitter/parser.h"
#include "runtime/tree.h"
#include "runtime/lexer.h"
#include "runtime/stack.h"
#include "runtime/parser.h"
#include "runtime/length.h"
#include "runtime/debugger.h"

/*
 *  Debugging
 */

#define DEBUG(...)                                                           \
  if (parser->lexer.debugger.debug_fn) {                                     \
    snprintf(parser->lexer.debug_buffer, TS_DEBUG_BUFFER_SIZE, __VA_ARGS__); \
    parser->lexer.debugger.debug_fn(parser->lexer.debugger.data,             \
                                    TSDebugTypeParse,                        \
                                    parser->lexer.debug_buffer);             \
  }

#define SYM_NAME(sym) parser->language->symbol_names[sym]

/*
 *  Private
 */

static const TSParseAction ERROR_ACTION = {
  .type = TSParseActionTypeError
};

static TSParseAction get_action(const TSLanguage *language, TSStateId state,
                                TSSymbol sym) {
  const TSParseAction *actions = (language->parse_table + (state * language->symbol_count))[sym];
  return actions ? actions[0] : ERROR_ACTION;
}

static TSLength break_down_left_stack(TSParser *parser, TSInputEdit edit) {
  ts_stack_shrink(&parser->right_stack, 0);

  TSLength prev_size = ts_stack_total_tree_size(&parser->stack);
  parser->total_chars =
      prev_size.chars + edit.chars_inserted - edit.chars_removed;
  TSLength left_subtree_end = prev_size;
  size_t right_subtree_start = parser->total_chars;

  for (;;) {
    TSTree *node = ts_stack_top_node(&parser->stack);
    if (!node)
      break;

    size_t child_count;
    TSTree **children = ts_tree_children(node, &child_count);
    if (left_subtree_end.chars < edit.position && !children && node->symbol != ts_builtin_sym_error)
      break;

    DEBUG("pop_left sym:%s, state:%u", SYM_NAME(node->symbol),
          ts_stack_top_state(&parser->stack));
    parser->stack.size--;
    left_subtree_end = ts_length_sub(left_subtree_end, ts_tree_total_size(node));

    size_t i = 0;
    for (; i < child_count && left_subtree_end.chars < edit.position; i++) {
      TSTree *child = children[i];
      TSStateId state = ts_stack_top_state(&parser->stack);
      TSParseAction action = get_action(parser->language, state, child->symbol);
      TSStateId next_state =
          ts_tree_is_extra(child) ? state : action.data.to_state;

      DEBUG("push_left sym:%s, state:%u", SYM_NAME(child->symbol), next_state);
      ts_stack_push(&parser->stack, next_state, child);
      left_subtree_end =
          ts_length_add(left_subtree_end, ts_tree_total_size(child));
    }

    for (size_t j = child_count - 1; j + 1 > i; j--) {
      TSTree *child = children[j];
      right_subtree_start -= ts_tree_total_size(child).chars;
      if (right_subtree_start < edit.position + edit.chars_inserted)
        break;

      DEBUG("push_right sym:%s", SYM_NAME(child->symbol));
      ts_stack_push(&parser->right_stack, 0, child);
    }

    ts_tree_release(node);
  }

  DEBUG("reuse_left chars:%lu, state:%u", left_subtree_end.chars,
        ts_stack_top_state(&parser->stack));
  return left_subtree_end;
}

static TSTree *break_down_right_stack(TSParser *parser) {
  TSStack *stack = &parser->right_stack;
  TSLength current_position = parser->lexer.current_position;
  TSStateId state = ts_stack_top_state(&parser->stack);

  size_t right_subtree_start =
      parser->total_chars - ts_stack_total_tree_size(stack).chars;

  for (;;) {
    TSTree *node = ts_stack_top_node(stack);
    if (!node)
      return NULL;

    if (right_subtree_start > current_position.chars)
      return NULL;

    TSParseAction action = get_action(parser->language, state, node->symbol);
    bool is_usable = (action.type != TSParseActionTypeError) &&
                     !ts_tree_is_extra(node) &&
                     !ts_tree_is_empty(node) &&
                     !ts_tree_is_fragile_left(node) &&
                     !ts_tree_is_fragile_right(node);
    if (is_usable && right_subtree_start == current_position.chars) {
      ts_stack_shrink(&parser->right_stack, parser->right_stack.size - 1);
      return node;
    }

    size_t child_count;
    TSTree **children = ts_tree_children(node, &child_count);

    DEBUG("pop_right sym:%s", SYM_NAME(node->symbol));
    stack->size--;
    right_subtree_start += ts_tree_total_size(node).chars;

    for (size_t i = child_count - 1; i + 1 > 0; i--) {
      if (right_subtree_start <= current_position.chars)
        break;

      TSTree *child = children[i];
      DEBUG("push_right sym:%s", SYM_NAME(child->symbol));
      ts_stack_push(stack, 0, child);
      right_subtree_start -= ts_tree_total_size(child).chars;
    }

    ts_tree_release(node);
  }
}

static TSTree *get_next_node(TSParser *parser, TSStateId lex_state) {
  TSTree *node;

  if ((node = break_down_right_stack(parser))) {
    DEBUG("reuse sym:%s, is_extra:%u, size:%lu", SYM_NAME(node->symbol),
          ts_tree_is_extra(node), ts_tree_total_size(node).chars);

    parser->lexer.token_start_position =
        ts_length_add(parser->lexer.current_position, node->padding);
    parser->lexer.token_end_position = parser->lexer.current_position =
        ts_length_add(parser->lexer.token_start_position, node->size);

    parser->lexer.lookahead = 0;
    parser->lexer.lookahead_size = 0;
    parser->lexer.advance_fn(&parser->lexer, 0);
  } else {
    node = parser->language->lex_fn(&parser->lexer, lex_state);
  }

  return node;
}

/*
 *  Parse Actions
 */

static void shift(TSParser *parser, TSStateId parse_state) {
  ts_stack_push(&parser->stack, parse_state, parser->lookahead);
  parser->lookahead = NULL;
}

static void shift_extra(TSParser *parser, TSStateId state) {
  ts_tree_set_extra(parser->lookahead);
  shift(parser, state);
}

static TSTree * reduce_helper(TSParser *parser, TSSymbol symbol, size_t child_count, bool extra, bool count_extra) {

  /*
   *  Walk down the stack to determine which symbols will be reduced.
   *  The child node count is known ahead of time, but some children
   *  may be ubiquitous tokens, which don't count.
   */
  TSStack *stack = &parser->stack;
  if (!count_extra) {
    for (size_t i = 0; i < child_count; i++) {
      if (child_count == stack->size)
        break;
      TSTree *child = stack->entries[stack->size - 1 - i].node;
      if (ts_tree_is_extra(child))
        child_count++;
    }
  }

  size_t start_index = stack->size - child_count;
  TSTree **children = calloc(child_count, sizeof(TSTree *));
  for (size_t i = 0; i < child_count; i++)
    children[i] = stack->entries[start_index + i].node;

  bool hidden = parser->language->hidden_symbol_flags[symbol];
  TSTree *parent = ts_tree_make_node(symbol, child_count, children, hidden);

  ts_stack_shrink(stack, start_index);
  TSStateId top_state = ts_stack_top_state(stack), state;
  if (extra)
    state = top_state;
  else
    state = get_action(parser->language, top_state, symbol).data.to_state;

  ts_stack_push(stack, state, parent);
  return parent;
}

static void reduce(TSParser *parser, TSSymbol symbol, size_t child_count) {
  reduce_helper(parser, symbol, child_count, false, false);
}

static void reduce_extra(TSParser *parser, TSSymbol symbol) {
  TSTree *reduced = reduce_helper(parser, symbol, 1, true, false);
  ts_tree_set_extra(reduced);
}

static void reduce_fragile(TSParser *parser, TSSymbol symbol, size_t child_count) {
  TSTree *reduced = reduce_helper(parser, symbol, child_count, false, false);
  ts_tree_set_fragile_left(reduced);
  ts_tree_set_fragile_right(reduced);
}

static void reduce_error(TSParser *parser, size_t child_count) {
  TSTree *reduced = reduce_helper(parser, ts_builtin_sym_error, child_count, false, true);
  reduced->size = ts_length_add(reduced->size, parser->lookahead->padding);
  parser->lookahead->padding = ts_length_zero();
  ts_tree_set_fragile_left(reduced);
  ts_tree_set_fragile_right(reduced);
}

static int handle_error(TSParser *parser) {
  size_t index_before_error = parser->stack.size - 1;

  for (;;) {

    /*
     *  Unwind the parse stack until a state is found in which an error is
     *  expected and the current lookahead token is expected afterwards.
     */
    for (size_t i = index_before_error; i + 1 > 0; i--) {
      TSStateId stack_state = parser->stack.entries[i].state;
      TSParseAction action_on_error = get_action(
        parser->language, stack_state, ts_builtin_sym_error);

      if (action_on_error.type == TSParseActionTypeShift) {
        TSStateId state_after_error = action_on_error.data.to_state;
        TSParseAction action_after_error = get_action(
            parser->language, state_after_error, parser->lookahead->symbol);

        if (action_after_error.type != TSParseActionTypeError) {
          DEBUG("recover state:%u, count:%lu", state_after_error, parser->stack.size - i);
          reduce_error(parser, parser->stack.size - i - 1);
          return 1;
        }
      }
    }

    /*
     *  If there is no state in the stack for which we can recover with the
     *  current lookahead token, advance to the next token.
     */
    DEBUG("skip token:%s", SYM_NAME(parser->lookahead->symbol));
    shift(parser, ts_stack_top_state(&parser->stack));
    parser->lookahead = get_next_node(parser, ts_lex_state_error);

    /*
     *  If the end of input is reached, exit.
     */
    if (parser->lookahead->symbol == ts_builtin_sym_end) {
      DEBUG("fail_to_recover");
      reduce_error(parser, parser->stack.size - index_before_error - 1);
      return 0;
    }
  }
}

static TSTree *finish(TSParser *parser) {
  reduce(parser, ts_builtin_sym_document, parser->stack.size);
  return parser->stack.entries[0].node;
}

/*
 *  Public
 */

TSParser ts_parser_make() {
  return (TSParser) { .lexer = ts_lexer_make(),
                      .stack = ts_stack_make(),
                      .right_stack = ts_stack_make() };
}

void ts_parser_destroy(TSParser *parser) {
  ts_stack_delete(&parser->stack);
  ts_stack_delete(&parser->right_stack);

  if (parser->lookahead)
    ts_tree_release(parser->lookahead);

  if (parser->lexer.debugger.release_fn)
    parser->lexer.debugger.release_fn(parser->lexer.debugger.data);
}

TSDebugger ts_parser_get_debugger(const TSParser *parser) {
  return parser->lexer.debugger;
}

void ts_parser_set_debugger(TSParser *parser, TSDebugger debugger) {
  if (parser->lexer.debugger.release_fn)
    parser->lexer.debugger.release_fn(parser->lexer.debugger.data);
  parser->lexer.debugger = debugger;
}

const TSTree *ts_parser_parse(TSParser *parser, TSInput input,
                              TSInputEdit *edit) {
  TSLength position;
  if (edit) {
    DEBUG("edit pos:%lu, inserted:%lu, deleted:%lu", edit->position,
          edit->chars_inserted, edit->chars_removed);
    position = break_down_left_stack(parser, *edit);
  } else {
    DEBUG("new_parse");
    ts_stack_shrink(&parser->stack, 0);
    position = ts_length_zero();
  }

  parser->lookahead = NULL;
  parser->lexer.input = input;
  ts_lexer_reset(&parser->lexer, position);

  for (;;) {
    TSStateId state = ts_stack_top_state(&parser->stack);
    if (!parser->lookahead)
      parser->lookahead = get_next_node(parser, parser->language->lex_states[state]);
    TSParseAction action = get_action(parser->language, state, parser->lookahead->symbol);

    DEBUG("lookahead state:%d, sym:%s", state, SYM_NAME(parser->lookahead->symbol));
    switch (action.type) {
      case TSParseActionTypeShift:
        if (parser->lookahead->symbol == ts_builtin_sym_error) {
          DEBUG("error_sym");
          if (!handle_error(parser))
            return finish(parser);
        } else {
          DEBUG("shift state:%u", action.data.to_state);
          shift(parser, action.data.to_state);
        }
        break;

      case TSParseActionTypeShiftExtra:
        DEBUG("shift_extra");
        shift_extra(parser, state);
        break;

      case TSParseActionTypeReduce:
        DEBUG("reduce sym:%s, count:%u", SYM_NAME(action.data.symbol), action.data.child_count);
        reduce(parser, action.data.symbol, action.data.child_count);
        break;

      case TSParseActionTypeReduceExtra:
        DEBUG("reduce_extra sym:%s", SYM_NAME(action.data.symbol));
        reduce_extra(parser, action.data.symbol);
        break;

      case TSParseActionTypeReduceFragile:
        DEBUG("reduce_fragile sym:%s, count:%u", SYM_NAME(action.data.symbol), action.data.child_count);
        reduce_fragile(parser, action.data.symbol, action.data.child_count);
        break;

      case TSParseActionTypeAccept:
        DEBUG("accept");
        return finish(parser);

      case TSParseActionTypeError:
        DEBUG("error_sym");
        if (!handle_error(parser))
          return finish(parser);
        break;

      default:
        return NULL;
    }
  }
}
