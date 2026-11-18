(comment) @comment

; Keys come first: where both patterns match, the earlier one wins.
(path (bare_key) @property)
(path (double_quoted_string) @property)
(path (single_quoted_string) @property)

(escape_sequence) @string.escape

[
  (double_quoted_string)
  (single_quoted_string)
  (double_fenced_string)
  (single_fenced_string)
] @string

(number) @number

[
  (true)
  (false)
] @boolean

(null) @constant.builtin

[
  "{"
  "}"
  "["
  "]"
] @punctuation.bracket

[
  ","
  ":"
  "."
] @punctuation.delimiter

[
  "-"
  "---"
] @punctuation.special
