// External scanner for the cnf grammar.
//
// cnf is line oriented and indentation sensitive, so the scanner owns
// everything that depends on the shape of a line:
//
//   - line ends, blank lines, and the INDENT / DEDENT tokens derived from the
//     prefix of the line that follows;
//   - comments, which are only comments at the start of a line or after
//     padding;
//   - the first bare key of a path, which needs lookahead to be told apart
//     from a number or a keyword (`1.5` is a number, `1.5: 2` is a path);
//   - the `- ` and `---` list markers, which are otherwise valid bare keys;
//   - fenced strings, whose content and closing delimiter are defined by the
//     prefix of the opening delimiter.

#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"

#include <string.h>

// The order must match `externals` in grammar.js.
enum TokenType {
  BOF,
  NEWLINE,
  INDENT,
  DEDENT,
  COMMENT,
  BARE_KEY,
  DASH,
  LIST_FENCE,
  SFENCE_OPEN,
  DFENCE_OPEN,
  FENCE_EOL,
  SFENCE_CONTENT,
  DFENCE_CONTENT,
  SFENCE_CLOSE,
  DFENCE_CLOSE,
  SFENCE_INLINE_CONTENT,
  DFENCE_INLINE_CONTENT,
  LINE_BREAK,
  ERROR_SENTINEL,
};

#define MAX_INDENT UINT16_MAX
#define MAX_DEPTH 256

typedef struct {
  // The prefix lengths of the open blocks, the top level's zero left out.
  Array(uint16_t) indents;
  // DEDENT tokens still owed after the last NEWLINE.
  uint16_t pending_dedents;
  // The prefix length of the fenced string block being read.
  uint16_t fence_indent;
  // The prefix character of the current top-level value: ' ' or '\t'.
  char indent_char;
  // Blank lines sat between a key and its block: a fence may not open it.
  bool no_fence;
  // The structural tokens of the line end under the lexer were produced, and
  // the line break itself is still to be consumed.
  bool line_done;
} Scanner;

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static inline bool is_pad(int32_t c) { return c == ' ' || c == '\t'; }

static inline bool is_bare(int32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static inline bool at_eol(TSLexer *lexer) {
  return lexer->eof(lexer) || lexer->lookahead == '\n' ||
         lexer->lookahead == '\r';
}

static inline uint16_t current_indent(const Scanner *scanner) {
  return scanner->indents.size > 0 ? *array_back(&scanner->indents) : 0;
}

// Consumes the padding left on the line and reports whether the line ends
// there. Nothing is given back: callers either accept or fail the token.
static bool rest_is_blank(TSLexer *lexer) {
  while (is_pad(lexer->lookahead)) {
    advance(lexer);
  }
  return at_eol(lexer);
}

// Looks past the line end the lexer sits on, and past the blank lines after
// it, at the prefix of the next line with content, then names what that
// prefix means: INDENT for a deeper line, NEWLINE otherwise, with one DEDENT
// owed for every block that the next line closes.
//
// These tokens are all empty and sit at the end of the line's content, so the
// nodes they close end there too. The line break itself is consumed later, as
// an extra, by scan_line_break.
//
// The checks hold during error recovery too: a token refused here would
// otherwise be retried in recovery mode, and accepted as if nothing happened.
static bool scan_line_end(Scanner *scanner, TSLexer *lexer,
                          const bool *valid_symbols) {
  lexer->mark_end(lexer);
  if (!lexer->eof(lexer)) {
    if (lexer->lookahead == '\r') {
      advance(lexer);
      if (lexer->lookahead != '\n') {
        return false;
      }
    }
    advance(lexer);
  }

  uint32_t indent = 0;
  bool spaces = false;
  bool tabs = false;
  bool blank = false;
  for (;;) {
    indent = 0;
    spaces = false;
    tabs = false;
    while (is_pad(lexer->lookahead)) {
      if (lexer->lookahead == ' ') {
        spaces = true;
      } else {
        tabs = true;
      }
      if (indent < MAX_INDENT) {
        indent++;
      }
      advance(lexer);
    }
    if (lexer->lookahead == '\r') {
      advance(lexer);
      if (lexer->lookahead != '\n') {
        break;
      }
    }
    if (lexer->lookahead == '\n') {
      advance(lexer);
      blank = true;
      continue;
    }
    if (lexer->eof(lexer)) {
      indent = 0;
    }
    break;
  }

  uint16_t top = current_indent(scanner);
  char indent_char = tabs ? '\t' : ' ';
  if (indent > 0) {
    // A prefix is all spaces or all tabs, and the same for the whole of a
    // top-level value.
    if (spaces && tabs) {
      return false;
    }
    if (top > 0 && scanner->indent_char != indent_char) {
      return false;
    }
  }

  if (indent > top) {
    if (!valid_symbols[INDENT] || scanner->indents.size >= MAX_DEPTH) {
      return false;
    }
    if (top == 0) {
      scanner->indent_char = indent_char;
    }
    array_push(&scanner->indents, (uint16_t)indent);
    // Only a key's block is restricted; a nested list fence, where a plain
    // line end is also possible, may follow blank lines.
    scanner->no_fence = blank && !valid_symbols[NEWLINE];
    scanner->line_done = true;
    lexer->result_symbol = INDENT;
    return true;
  }

  if (!valid_symbols[NEWLINE]) {
    return false;
  }
  uint32_t size = scanner->indents.size;
  uint32_t pops = 0;
  while (pops < size && scanner->indents.contents[size - 1 - pops] > indent) {
    pops++;
  }
  uint16_t landing =
    pops < size ? scanner->indents.contents[size - 1 - pops] : 0;
  if (landing != indent) {
    // A block only closes at the exact prefix of one of its ancestors.
    return false;
  }
  scanner->pending_dedents = (uint16_t)pops;
  scanner->no_fence = false;
  scanner->line_done = true;
  lexer->result_symbol = NEWLINE;
  return true;
}

// Consumes the line break the lexer sits on and the blank lines after it.
static bool scan_line_break(TSLexer *lexer) {
  if (lexer->lookahead == '\r') {
    advance(lexer);
    if (lexer->lookahead != '\n') {
      return false;
    }
  }
  advance(lexer);
  for (;;) {
    lexer->mark_end(lexer);
    while (is_pad(lexer->lookahead)) {
      advance(lexer);
    }
    if (lexer->lookahead == '\r') {
      advance(lexer);
      if (lexer->lookahead != '\n') {
        break;
      }
    }
    if (lexer->lookahead == '\n') {
      advance(lexer);
      continue;
    }
    if (lexer->eof(lexer)) {
      lexer->mark_end(lexer);
    }
    break;
  }
  lexer->result_symbol = LINE_BREAK;
  return true;
}

// Skips the blank lines that open the file. The first line with content must
// not be indented.
static bool scan_bof(TSLexer *lexer) {
  uint32_t indent = 0;
  for (;;) {
    lexer->mark_end(lexer);
    indent = 0;
    while (is_pad(lexer->lookahead)) {
      indent++;
      advance(lexer);
    }
    if (lexer->lookahead == '\r') {
      advance(lexer);
      if (lexer->lookahead != '\n') {
        break;
      }
    }
    if (lexer->lookahead == '\n') {
      advance(lexer);
      continue;
    }
    if (lexer->eof(lexer)) {
      lexer->mark_end(lexer);
      indent = 0;
    }
    break;
  }
  if (indent > 0) {
    return false;
  }
  lexer->result_symbol = BOF;
  return true;
}

static bool scan_comment(TSLexer *lexer) {
  while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
    if (lexer->lookahead == '\r') {
      // Leave the CR of a CR LF pair to the line end.
      lexer->mark_end(lexer);
      advance(lexer);
      if (lexer->lookahead == '\n') {
        lexer->result_symbol = COMMENT;
        return true;
      }
      continue;
    }
    advance(lexer);
  }
  lexer->mark_end(lexer);
  lexer->result_symbol = COMMENT;
  return true;
}

// Reads a token that starts on a bare character: the `-` of a dashed list
// item, the `---` of a list fence, or the first bare key of a path.
static bool scan_bare(Scanner *scanner, TSLexer *lexer,
                      const bool *valid_symbols) {
  bool dashes = lexer->lookahead == '-';
  uint32_t length = 1;
  advance(lexer);
  lexer->mark_end(lexer);

  // A dash opens a list item only before a blank or the end of the line;
  // anywhere else it spells a key or a number.
  if (dashes && valid_symbols[DASH] &&
      (is_pad(lexer->lookahead) || at_eol(lexer))) {
    lexer->result_symbol = DASH;
    return true;
  }

  while (is_bare(lexer->lookahead)) {
    dashes = dashes && lexer->lookahead == '-';
    length++;
    advance(lexer);
  }
  lexer->mark_end(lexer);

  if (dashes && length == 3 && (is_pad(lexer->lookahead) || at_eol(lexer))) {
    // A list fence sits alone on its line.
    if (valid_symbols[LIST_FENCE] && !scanner->no_fence &&
        rest_is_blank(lexer)) {
      lexer->result_symbol = LIST_FENCE;
      return true;
    }
    return false;
  }

  if (!valid_symbols[BARE_KEY]) {
    return false;
  }
  // The word is a key only when it opens a path that reaches its colon;
  // otherwise it is left for the number and keyword tokens.
  for (;;) {
    if (lexer->lookahead == ':') {
      lexer->result_symbol = BARE_KEY;
      return true;
    }
    if (lexer->lookahead != '.') {
      return false;
    }
    advance(lexer);
    if (is_bare(lexer->lookahead)) {
      while (is_bare(lexer->lookahead)) {
        advance(lexer);
      }
    } else if (lexer->lookahead == '"') {
      advance(lexer);
      while (lexer->lookahead != '"') {
        if (lexer->lookahead == '\\') {
          advance(lexer);
        }
        if (at_eol(lexer)) {
          return false;
        }
        advance(lexer);
      }
      advance(lexer);
    } else if (lexer->lookahead == '\'') {
      advance(lexer);
      while (lexer->lookahead != '\'') {
        if (at_eol(lexer)) {
          return false;
        }
        advance(lexer);
      }
      advance(lexer);
    } else {
      return false;
    }
  }
}

// Opens a fenced string block: the delimiter alone on its line.
static bool scan_fence_open(Scanner *scanner, TSLexer *lexer,
                            const bool *valid_symbols) {
  int32_t quote = lexer->lookahead;
  enum TokenType symbol = quote == '"' ? DFENCE_OPEN : SFENCE_OPEN;
  if (!valid_symbols[symbol] || scanner->no_fence) {
    return false;
  }
  uint32_t column = lexer->get_column(lexer);
  for (int i = 0; i < 3; i++) {
    if (lexer->lookahead != quote) {
      return false;
    }
    advance(lexer);
  }
  lexer->mark_end(lexer);
  if (!rest_is_blank(lexer)) {
    return false;
  }
  scanner->fence_indent = column < MAX_INDENT ? (uint16_t)column : MAX_INDENT;
  lexer->result_symbol = symbol;
  return true;
}

// Reads inside a fenced string block: either a run of content, or the closing
// delimiter - a line holding only the delimiter at the prefix of the opening
// one. In the double-quoted family the content stops at each backslash, which
// the grammar reads as an escape sequence.
static bool scan_fence_block(Scanner *scanner, TSLexer *lexer,
                             const bool *valid_symbols, int32_t quote) {
  enum TokenType content = quote == '"' ? DFENCE_CONTENT : SFENCE_CONTENT;
  enum TokenType close = quote == '"' ? DFENCE_CLOSE : SFENCE_CLOSE;
  bool has_content = false;
  bool line_start = lexer->get_column(lexer) == 0;

  for (;;) {
    if (line_start) {
      line_start = false;
      // Content read so far ends before this line if the line closes the
      // fence.
      lexer->mark_end(lexer);
      uint32_t indent = 0;
      bool mixed = false;
      while (is_pad(lexer->lookahead) &&
             (has_content || indent < scanner->fence_indent)) {
        if (indent < scanner->fence_indent &&
            lexer->lookahead != scanner->indent_char) {
          mixed = true;
        }
        if (has_content) {
          advance(lexer);
        } else {
          // The fence's own prefix is not part of the token that starts on
          // this line.
          skip(lexer);
        }
        indent++;
      }
      if (mixed || (indent < scanner->fence_indent && !at_eol(lexer))) {
        // The line does not carry the fence's prefix, and it is not a blank
        // one: the content ends before it, and nothing can be read here.
        lexer->result_symbol = content;
        return has_content && valid_symbols[content];
      }
      if (indent == scanner->fence_indent && lexer->lookahead == quote) {
        uint32_t quotes = 0;
        while (quotes < 3 && lexer->lookahead == quote) {
          advance(lexer);
          quotes++;
        }
        if (quotes == 3) {
          if (!has_content) {
            lexer->mark_end(lexer);
          }
          if (rest_is_blank(lexer)) {
            if (has_content) {
              lexer->result_symbol = content;
              return true;
            }
            if (!valid_symbols[close]) {
              return false;
            }
            lexer->result_symbol = close;
            return true;
          }
        }
        has_content = true;
      }
    }

    while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
      if (quote == '"' && lexer->lookahead == '\\') {
        lexer->mark_end(lexer);
        lexer->result_symbol = content;
        return has_content && valid_symbols[content];
      }
      advance(lexer);
      has_content = true;
    }
    if (lexer->eof(lexer)) {
      lexer->mark_end(lexer);
      lexer->result_symbol = content;
      return has_content && valid_symbols[content];
    }
    advance(lexer);
    has_content = true;
    line_start = true;
  }
}

// Reads the content of a one-line fenced string, up to the first closing
// delimiter. One or two quotes in a row are content.
static bool scan_fence_inline(TSLexer *lexer, int32_t quote) {
  bool has_content = false;
  lexer->result_symbol =
    quote == '"' ? DFENCE_INLINE_CONTENT : SFENCE_INLINE_CONTENT;
  for (;;) {
    if (at_eol(lexer) || (quote == '"' && lexer->lookahead == '\\')) {
      break;
    }
    if (lexer->lookahead == quote) {
      lexer->mark_end(lexer);
      uint32_t quotes = 0;
      while (quotes < 3 && lexer->lookahead == quote) {
        advance(lexer);
        quotes++;
      }
      if (quotes == 3) {
        return has_content;
      }
      has_content = true;
      continue;
    }
    advance(lexer);
    has_content = true;
  }
  lexer->mark_end(lexer);
  return has_content;
}

static bool scan(Scanner *scanner, TSLexer *lexer, const bool *valid_symbols) {
  // During error recovery every symbol is valid: only the line structure and
  // the comments are produced then, without the strict checks.
  bool recovery = valid_symbols[ERROR_SENTINEL];

  if (scanner->pending_dedents > 0) {
    if (valid_symbols[DEDENT]) {
      scanner->pending_dedents--;
      if (scanner->indents.size > 0) {
        scanner->indents.size--;
      }
      lexer->result_symbol = DEDENT;
      return true;
    }
    // The parser is not where the scanner thought it was: keep the stack in
    // step with the text and move on.
    while (scanner->pending_dedents > 0) {
      scanner->pending_dedents--;
      if (scanner->indents.size > 0) {
        scanner->indents.size--;
      }
    }
  }

  if (!recovery) {
    if (valid_symbols[FENCE_EOL]) {
      while (is_pad(lexer->lookahead)) {
        advance(lexer);
      }
      if (lexer->lookahead == '\r') {
        advance(lexer);
      }
      if (lexer->lookahead == '\n') {
        advance(lexer);
      } else if (!lexer->eof(lexer)) {
        return false;
      }
      lexer->result_symbol = FENCE_EOL;
      return true;
    }
    if (valid_symbols[SFENCE_CONTENT] || valid_symbols[SFENCE_CLOSE]) {
      return scan_fence_block(scanner, lexer, valid_symbols, '\'');
    }
    if (valid_symbols[DFENCE_CONTENT] || valid_symbols[DFENCE_CLOSE]) {
      return scan_fence_block(scanner, lexer, valid_symbols, '"');
    }
    if (valid_symbols[SFENCE_INLINE_CONTENT]) {
      return scan_fence_inline(lexer, '\'');
    }
    if (valid_symbols[DFENCE_INLINE_CONTENT]) {
      return scan_fence_inline(lexer, '"');
    }
    if (valid_symbols[BOF]) {
      return scan_bof(lexer);
    }
  }

  bool padded = false;
  while (is_pad(lexer->lookahead)) {
    skip(lexer);
    padded = true;
  }

  if (at_eol(lexer)) {
    if (scanner->line_done) {
      // The final line break may be omitted.
      if (lexer->eof(lexer)) {
        return false;
      }
      scanner->line_done = false;
      return scan_line_break(lexer);
    }
    if (valid_symbols[NEWLINE] || valid_symbols[INDENT]) {
      return scan_line_end(scanner, lexer, valid_symbols);
    }
    return false;
  }

  if (lexer->lookahead == '#') {
    // A comment stands at the start of its line or after padding; glued to a
    // value it is not one.
    if (valid_symbols[COMMENT] && (padded || lexer->get_column(lexer) == 0)) {
      return scan_comment(lexer);
    }
    return false;
  }

  if (recovery) {
    return false;
  }

  if (lexer->lookahead == '\'' || lexer->lookahead == '"') {
    return scan_fence_open(scanner, lexer, valid_symbols);
  }

  if (is_bare(lexer->lookahead) &&
      (valid_symbols[DASH] || valid_symbols[LIST_FENCE] ||
       valid_symbols[BARE_KEY])) {
    return scan_bare(scanner, lexer, valid_symbols);
  }

  return false;
}

void *tree_sitter_cnf_external_scanner_create(void) {
  Scanner *scanner = ts_calloc(1, sizeof(Scanner));
  array_init(&scanner->indents);
  return scanner;
}

void tree_sitter_cnf_external_scanner_destroy(void *payload) {
  Scanner *scanner = (Scanner *)payload;
  array_delete(&scanner->indents);
  ts_free(scanner);
}

unsigned tree_sitter_cnf_external_scanner_serialize(void *payload,
                                                    char *buffer) {
  Scanner *scanner = (Scanner *)payload;
  unsigned size = 0;
  memcpy(&buffer[size], &scanner->pending_dedents, sizeof(uint16_t));
  size += sizeof(uint16_t);
  memcpy(&buffer[size], &scanner->fence_indent, sizeof(uint16_t));
  size += sizeof(uint16_t);
  buffer[size++] = scanner->indent_char;
  buffer[size++] = (char)scanner->no_fence;
  buffer[size++] = (char)scanner->line_done;
  // MAX_DEPTH keeps the stack well inside the serialization buffer.
  unsigned bytes = scanner->indents.size * sizeof(uint16_t);
  if (bytes > 0) {
    memcpy(&buffer[size], scanner->indents.contents, bytes);
    size += bytes;
  }
  return size;
}

void tree_sitter_cnf_external_scanner_deserialize(void *payload,
                                                  const char *buffer,
                                                  unsigned length) {
  Scanner *scanner = (Scanner *)payload;
  array_clear(&scanner->indents);
  scanner->pending_dedents = 0;
  scanner->fence_indent = 0;
  scanner->indent_char = 0;
  scanner->no_fence = false;
  scanner->line_done = false;
  if (length == 0) {
    return;
  }
  unsigned size = 0;
  memcpy(&scanner->pending_dedents, &buffer[size], sizeof(uint16_t));
  size += sizeof(uint16_t);
  memcpy(&scanner->fence_indent, &buffer[size], sizeof(uint16_t));
  size += sizeof(uint16_t);
  scanner->indent_char = buffer[size++];
  scanner->no_fence = buffer[size++] != 0;
  scanner->line_done = buffer[size++] != 0;
  unsigned count = (length - size) / sizeof(uint16_t);
  if (count > 0) {
    array_reserve(&scanner->indents, count);
    memcpy(scanner->indents.contents, &buffer[size], count * sizeof(uint16_t));
    scanner->indents.size = count;
  }
}

bool tree_sitter_cnf_external_scanner_scan(void *payload, TSLexer *lexer,
                                           const bool *valid_symbols) {
  return scan((Scanner *)payload, lexer, valid_symbols);
}
