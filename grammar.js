/**
 * @file Tree-sitter grammar for cnf, a configuration file format focused on
 * human readability and correctness (https://github.com/canonical/cnf).
 * @author Canonical Ltd
 * @license MIT
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

// The spec forbids C0 controls, U+2028 and U+2029 inside quoted strings. The
// tab is tolerated here because the reference implementation and the fenced
// conformance cases accept it.
const SQ_CONTENT = /[^'\u0000-\u0008\u000a-\u001f\u2028\u2029]+/;
const DQ_CONTENT = /[^"\\\u0000-\u0008\u000a-\u001f\u2028\u2029]+/;

const HEX = '[0-9a-fA-F]';
// Four hex digits outside of the surrogate range U+D800 to U+DFFF.
const SCALAR4 = `([0-9a-cA-CefEF]${HEX}{3}|[dD][0-7]${HEX}{2})`;

const ESCAPE = seq('\\', choice(
  /["\\nrtbf]/,
  new RegExp(`x${HEX}{2}`),
  new RegExp(`u${SCALAR4}`),
  // One to six hex digits, at most U+10FFFF, and no surrogates.
  new RegExp(
    'u\\{(' + [
      `${HEX}{1,3}`,
      SCALAR4,
      `0${SCALAR4}`,
      `[1-9a-fA-F]${HEX}{4}`,
      `00${SCALAR4}`,
      `0[1-9a-fA-F]${HEX}{4}`,
      `10${HEX}{4}`,
    ].join('|') + ')\\}',
  ),
));

const BARE = /[A-Za-z0-9_-]+/;

const NUMBER = seq(
  optional('-'),
  choice(
    /0x[0-9a-fA-F]+/,
    /0o[0-7]+/,
    /0b[01]+/,
    seq(
      choice('0', /[1-9][0-9]*/),
      optional(/\.[0-9]+/),
      optional(/e[+-]?[0-9]+/),
    ),
  ),
);

/**
 * Comma separated list without a trailing comma.
 *
 * @param {RuleOrLiteral} rule
 */
function commaSep(rule) {
  return optional(seq(rule, repeat(seq(',', rule))));
}

export default grammar({
  name: 'cnf',

  // The order must match `enum TokenType` in src/scanner.c.
  externals: $ => [
    $._bof,
    $._newline,
    $._indent,
    $._dedent,
    $.comment,
    $.bare_key,
    $._dash,
    $._list_fence,
    $._sfence_open,
    $._dfence_open,
    $._fence_eol,
    $._sfence_content,
    $._dfence_content,
    $._sfence_close,
    $._dfence_close,
    $._sfence_inline_content,
    $._dfence_inline_content,
    $._line_break,
    $._error_sentinel,
  ],

  // Line ends are significant and comments may only sit at the places the
  // grammar names, so neither floats freely. The scanner closes each line with
  // empty tokens placed right after its content, and only then hands out the
  // line break itself as an extra, which keeps it out of the nodes it follows.
  extras: $ => [/[ \t]/, $._line_break],

  rules: {
    // file = mapcomment [ topvalue mapcomment ]
    document: $ => seq(
      $._bof,
      optional($._comments),
      optional(choice(
        $.block_map,
        $.block_list,
        $.fenced_list,
        seq(
          choice($._inline_line, $._fenced_string_line),
          optional($._comments),
        ),
      )),
    ),

    _comments: $ => repeat1($._comment_line),
    _comment_line: $ => seq($.comment, $._newline),

    _inline_line: $ => seq($._inline, optional($.comment), $._newline),
    _fenced_string_line: $ => seq($._fenced_string_block, $._newline),

    // Block forms

    block_map: $ => seq($.pair, repeat(choice($.pair, $._comment_line))),

    pair: $ => seq(
      field('key', $.path),
      token.immediate(':'),
      choice(
        seq(field('value', $._inline), optional($.comment), $._newline),
        seq(
          $._indent,
          choice(
            seq(
              optional($._comments),
              field('value', choice($.block_map, $.block_list)),
            ),
            seq(field('value', $._fenced_string_block), $._newline),
            field('value', $.fenced_list),
          ),
          $._dedent,
        ),
      ),
    ),

    block_list: $ => seq(
      $.list_item,
      repeat(choice($.list_item, $._comment_line)),
    ),

    list_item: $ => seq(
      alias($._dash, '-'),
      choice($._inline, alias($._inline_pair, $.pair)),
      optional($.comment),
      $._newline,
    ),

    // listfence = lfence nl 1*( ( inline / fenceblock ) nl lfence nl )
    fenced_list: $ => seq(
      alias($._list_fence, '---'),
      repeat1(seq(
        choice(
          seq(
            $._newline,
            optional($._comments),
            choice(
              $.block_map,
              $.block_list,
              seq(
                choice($._inline_line, $._fenced_string_line),
                optional($._comments),
              ),
            ),
          ),
          seq($._indent, optional($._comments), $.fenced_list, $._dedent),
        ),
        alias($._list_fence, '---'),
      )),
      $._newline,
      optional($._comments),
    ),

    _fenced_string_block: $ => choice(
      alias($._single_fenced_block, $.single_fenced_string),
      alias($._double_fenced_block, $.double_fenced_string),
    ),

    _single_fenced_block: $ => seq(
      alias($._sfence_open, '\'\'\''),
      $._fence_eol,
      optional(alias($._sfence_content, $.string_content)),
      alias($._sfence_close, '\'\'\''),
    ),

    _double_fenced_block: $ => seq(
      alias($._dfence_open, '"""'),
      $._fence_eol,
      repeat(choice(
        alias($._dfence_content, $.string_content),
        alias($._block_escape, $.escape_sequence),
        alias($._line_continuation, $.escape_sequence),
      )),
      alias($._dfence_close, '"""'),
    ),

    // Unlike the escapes of a one-line string these are not immediate tokens:
    // one may open a line, right after the prefix the fence does not own.
    _block_escape: _ => token(ESCAPE),
    _line_continuation: _ => /\\\r?\n/,

    // Inline forms

    _inline: $ => choice(
      $.double_quoted_string,
      $.single_quoted_string,
      alias($._double_fenced_inline, $.double_fenced_string),
      alias($._single_fenced_inline, $.single_fenced_string),
      $.number,
      $.true,
      $.false,
      $.null,
      $.map,
      $.list,
    ),

    map: $ => seq('{', commaSep(alias($._inline_pair, $.pair)), '}'),

    list: $ => seq('[', commaSep($._inline), ']'),

    _inline_pair: $ => seq(
      field('key', $.path),
      token.immediate(':'),
      field('value', $._inline),
    ),

    // Keys and paths: nothing may sit between the keys, the dots and the colon.

    path: $ => seq(
      $._key,
      repeat(seq(token.immediate('.'), $._immediate_key)),
    ),

    _key: $ => choice(
      $.bare_key,
      $.double_quoted_string,
      $.single_quoted_string,
    ),

    _immediate_key: $ => choice(
      alias(token.immediate(BARE), $.bare_key),
      alias($._immediate_double_quoted_string, $.double_quoted_string),
      alias($._immediate_single_quoted_string, $.single_quoted_string),
    ),

    // Strings

    double_quoted_string: $ => seq('"', repeat($._dq_part), '"'),

    _immediate_double_quoted_string: $ => seq(
      alias(token.immediate('"'), '"'),
      repeat($._dq_part),
      '"',
    ),

    _dq_part: $ => choice(
      alias($._dq_content, $.string_content),
      $.escape_sequence,
    ),

    single_quoted_string: $ => seq(
      '\'',
      optional(alias($._sq_content, $.string_content)),
      '\'',
    ),

    _immediate_single_quoted_string: $ => seq(
      alias(token.immediate('\''), '\''),
      optional(alias($._sq_content, $.string_content)),
      '\'',
    ),

    _double_fenced_inline: $ => seq(
      '"""',
      repeat(choice(
        alias($._dfence_inline_content, $.string_content),
        $.escape_sequence,
      )),
      token.immediate('"""'),
    ),

    _single_fenced_inline: $ => seq(
      '\'\'\'',
      optional(alias($._sfence_inline_content, $.string_content)),
      token.immediate('\'\'\''),
    ),

    _dq_content: _ => token.immediate(prec(1, DQ_CONTENT)),
    _sq_content: _ => token.immediate(prec(1, SQ_CONTENT)),

    escape_sequence: _ => token.immediate(ESCAPE),

    // Scalars

    number: _ => token(NUMBER),
    true: _ => 'true',
    false: _ => 'false',
    null: _ => 'null',
  },
});
