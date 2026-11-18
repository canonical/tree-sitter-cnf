# tree-sitter-cnf

A [tree-sitter](https://tree-sitter.github.io) grammar for
[cnf](https://github.com/canonical/cnf), a configuration file format focused
on human readability and correctness. It follows the 1.0rc1 specification.

```
service:
    name: "now"       # inline comment
    ports: [80, 443]
    env:
        - "A=1"
        - "B=2"
    script:
        """
        #!/bin/sh
        echo "not today"
        """
```

## Syntax tree

| Node | Syntax |
|---|---|
| `document` | The whole file: comments and at most one top-level value. |
| `block_map` | `key: value` lines at one indentation prefix. |
| `map` | Inline map, `{a: 1, b: 2}`. |
| `pair` | A `path: value` entry of a `block_map`, a `map`, or a dashed `list_item`. Fields `key` and `value`. |
| `path` | One or more keys joined by `.`: `bare_key`, `double_quoted_string`, `single_quoted_string`. |
| `block_list` | Dashed list. Each line is a `list_item` holding a value or a `pair`. |
| `fenced_list` | `---` delimited list. Its children are the elements, one value each. |
| `list` | Inline list, `[1, 2]`. |
| `double_quoted_string`, `single_quoted_string` | `"..."` and `'...'`. |
| `double_fenced_string`, `single_fenced_string` | `"""..."""` and `'''...'''`, on one line or as a block. |
| `string_content`, `escape_sequence` | The pieces of a string. A `\` that ends a line of a `"""` block is an `escape_sequence` too. |
| `number`, `true`, `false`, `null` | Scalars. |
| `comment` | Standalone or inline `#` comment. |

Comments are regular children of the node they sit in, not extras: an inline
comment belongs to its `pair` or `list_item`, a standalone one to the
enclosing block. The comments that precede the first line of a block under a
key are children of that key's `pair`.

Nodes end where their text ends: a `pair` does not include its line break,
nor the blank lines after it.

## Strictness

Inputs that the spec's grammar rejects produce `ERROR` or `MISSING` nodes,
including the layout rules: indentation prefixes that mix tabs and spaces,
blocks that close at a prefix that is no ancestor's, misplaced comments, and
blank lines or comments between a key and its fence.

The rules below need interpretation of the data, so they are left to the
consumers of the tree:

- duplicate keys, and maps expanded more than once;
- nested fenced lists using different indents within one parent fence.

Known deviations from the reference implementation
([go-cnf](https://github.com/canonical/go-cnf)):

- Padding at the end of a line is accepted after values and fence
  delimiters, as the ABNF's `nl = pad NL` allows; go-cnf rejects it.
- A comment at a fenced list's own indent, after its closing `---`, is
  accepted as part of the `fenced_list`.
- A `---` line inside a fenced string block that is itself an element of a
  fenced list is string content, as in the ABNF; go-cnf splits the element
  there.
- Literal tabs are accepted inside quoted strings (as go-cnf does), while the
  other C0 controls, U+2028 and U+2029 are rejected, per the ABNF.

## License

MIT, see [LICENSE](LICENSE).

## Development

```sh
tree-sitter generate   # after editing grammar.js
tree-sitter test       # runs test/corpus
tree-sitter parse examples/example.cnf
```

`src/scanner.c` is the hand-written external scanner. It produces the line
ends, the indentation tokens, comments, list markers, the first key of a
path, and fenced string blocks; its token list must stay in the order of
`externals` in `grammar.js`.
