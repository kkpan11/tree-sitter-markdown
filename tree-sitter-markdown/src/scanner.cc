#include <tree_sitter/parser.h>
#include <cctype>
#include <cassert>
#include <vector>
#include <cstring>
#include <algorithm>
#include <iostream>

using std::vector;
using std::memcpy;

namespace TreeSitterMarkdown {

// For explanation of the tokens see grammar.js
enum TokenType {
    LINE_ENDING,
    BLOCK_CLOSE,
    BLOCK_CONTINUATION,
    BLOCK_QUOTE_CONTINUATION,
    BLOCK_QUOTE_START,
    INDENTED_CHUNK_START,
    ATX_H1_MARKER,
    ATX_H2_MARKER,
    ATX_H3_MARKER,
    ATX_H4_MARKER,
    ATX_H5_MARKER,
    ATX_H6_MARKER,
    SETEXT_H1_UNDERLINE,
    SETEXT_H2_UNDERLINE,
    THEMATIC_BREAK,
    LIST_MARKER_MINUS,
    LIST_MARKER_PLUS,
    LIST_MARKER_STAR,
    LIST_MARKER_PARENTHESIS,
    LIST_MARKER_DOT,
    LIST_MARKER_MINUS_DONT_INTERRUPT,
    LIST_MARKER_PLUS_DONT_INTERRUPT,
    LIST_MARKER_STAR_DONT_INTERRUPT,
    LIST_MARKER_PARENTHESIS_DONT_INTERRUPT,
    LIST_MARKER_DOT_DONT_INTERRUPT,
    FENCED_CODE_BLOCK_START_BACKTICK,
    FENCED_CODE_BLOCK_START_TILDE,
    BLANK_LINE_START,
    FENCED_CODE_BLOCK_END_BACKTICK,
    FENCED_CODE_BLOCK_END_TILDE,
    HTML_BLOCK_1_START,
    HTML_BLOCK_1_END,
    HTML_BLOCK_2_START,
    HTML_BLOCK_3_START,
    HTML_BLOCK_4_START,
    HTML_BLOCK_5_START,
    HTML_BLOCK_6_START,
    HTML_BLOCK_7_START,
    OPEN_BLOCK,
    OPEN_BLOCK_DONT_INTERRUPT_PARAGRAPH,
    CLOSE_BLOCK,
    NO_INDENTED_CHUNK,
    SPLIT_TOKEN,
    SOFT_LINE_BREAK_MARKER,
    ERROR,
    TRIGGER_ERROR,
};

// Description of a block on the block stack.
//
// LIST_ITEM is a list item with minimal indentation (content begins at indent level 2) while
// LIST_ITEM_MAX_INDENTATION represents a list item with maximal indentation without being
// considered a indented code block.
//
// ANONYMOUS represents any block that whose close is not handled by the external scanner. (Usually
// opened using OPEN_BLOCK and closed using CLOSE_BLOCK).
enum Block : uint8_t {
    BLOCK_QUOTE,
    INDENTED_CODE_BLOCK,
    LIST_ITEM = 2,
    LIST_ITEM_MAX_INDENTATION = 17,
    FENCED_CODE_BLOCK,
    ANONYMOUS
};

// Returns true if the block represents a list item
bool is_list_item(Block block) {
    return block >= LIST_ITEM && block <= LIST_ITEM_MAX_INDENTATION;
}

// Returns the indentation level which lines of a list item should have at minimum. Should only be
// called with blocks for which `is_list_item` returns true. 
uint8_t list_item_indentation(Block block) {
    return block - LIST_ITEM + 2;
}

const size_t NUM_HTML_TAG_NAMES_RULE_1 = 3;
const char* HTML_TAG_NAMES_RULE_1[NUM_HTML_TAG_NAMES_RULE_1] = { "pre", "script", "style" };
const size_t NUM_HTML_TAG_NAMES_RULE_7 = 62;
const char* HTML_TAG_NAMES_RULE_7[NUM_HTML_TAG_NAMES_RULE_7] = {
    "address", "article", "aside", "base", "basefont", "blockquote", "body", "caption", "center",
    "col", "colgroup", "dd", "details", "dialog", "dir", "div", "dl", "dt", "fieldset", "figcaption",
    "figure", "footer", "form", "frame", "frameset", "h1", "h2", "h3", "h4", "h5", "h6", "head",
    "header", "hr", "html", "iframe", "legend", "li", "link", "main", "menu", "menuitem", "nav",
    "noframes", "ol", "optgroup", "option", "p", "param", "section", "source", "summary", "table",
    "tbody", "td", "tfoot", "th", "thead", "title", "tr", "track", "ul"
};

// State bitflags used with `Scanner.state`

// Currently matching (at the beginning of a line)
const uint8_t STATE_MATCHING = 0x1 << 0;
// Last line break was inside a paragraph
const uint8_t STATE_WAS_SOFT_LINE_BREAK = 0x1 << 1;
// Number of consecutive SPLIT_TOKEN emitted
const uint8_t STATE_SPLIT_TOKEN_COUNT = 0x3 << 2;
// Block should be closed after next line break
const uint8_t STATE_CLOSE_BLOCK = 0x1 << 4;
// Paragraph was just closed. Error if no new block opens
const uint8_t STATE_NEED_OPEN_BLOCK = 0x1 << 5; 
// Basically the same as STATE_NEED_OPEN_BLOCK. I am actually not sure about the difference. This
// shoudl be investigated.
const uint8_t STATE_JUST_CLOSED = 0x1 << 6;

struct Scanner {

    // A stack of open blocks in the current parse state
    vector<Block> open_blocks;
    // Parser state flags
    uint8_t state;
    // Number of blocks that have been matched so far. Only changes during matching and is reset
    // after every line ending
    uint8_t matched;
    // Consumed but "unused" indentation. Sometimes a tab needs to be "split" to be used in
    // multiple tokens.
    uint8_t indentation;
    // The current column. Used to decide how many spaces a tab should equal
    uint8_t column;
    // The delimiter length of the currently open fenced code block
    uint8_t fenced_code_block_delimiter_length;

    Scanner() {
        assert(sizeof(Block) == sizeof(char));
        assert(ATX_H6_MARKER == ATX_H1_MARKER + 5);
        deserialize(NULL, 0);
    }

    // Write the whole state of a Scanner to a byte buffer
    unsigned serialize(char *buffer) {
        size_t i = 0;
        buffer[i++] = state;
        buffer[i++] = matched;
        buffer[i++] = indentation;
        buffer[i++] = column;
        buffer[i++] = fenced_code_block_delimiter_length;
        size_t blocks_count = open_blocks.size();
        if (blocks_count > UINT8_MAX - i) blocks_count = UINT8_MAX - i;
        if (blocks_count > 0) {
            memcpy(&buffer[i], open_blocks.data(), blocks_count);
            i += blocks_count;
        }
        return i;
    }

    // Read the whole state of a Scanner from a byte buffer
    // `serizalize` and `deserialize` should be fully symmetric.
    void deserialize(const char *buffer, unsigned length) {
        open_blocks.clear();
        state = 0;
        matched = 0;
        indentation = 0;
        column = 0;
        fenced_code_block_delimiter_length = 0;
        if (length > 0) {
            size_t i = 0;
            state = buffer[i++];
            matched = buffer[i++];
            indentation = buffer[i++];
            column = buffer[i++];
            fenced_code_block_delimiter_length = buffer[i++];
            size_t blocks_count = length - i;
            open_blocks.resize(blocks_count);
            if (blocks_count > 0) {
                memcpy(open_blocks.data(), &buffer[i], blocks_count);
            }
        }
    }

    // Advance the lexer one character
    // Also keeps track of the current column, counting tabs as spaces with tab stop 4
    // See https://github.github.com/gfm/#tabs
    size_t advance(TSLexer *lexer) {
        size_t size = 1;
        if (lexer->lookahead == '\t') {
            size = (column % 4 == 0) ? 4 : (4 - column % 4);
        }
        column += size;
        lexer->advance(lexer, false);
        return size;
    }

    // Convenience function to emit the error token. This is done to stop invalid parse branches.
    // Specifically:
    // 1. When encountering a newline after a line break that ended a paragraph, and no new block
    //    has been opened.
    // 2. When encountering a new block after a soft line break.
    // 3. When a `$._trigger_error` token is valid, which is used to stop parse branches through
    //    normal tree-sitter grammar rules.
    //
    // See also the `$._soft_line_break` and `$._paragraph_end_newline` tokens in grammar.js
    bool error(TSLexer *lexer) {
        lexer->result_symbol = ERROR;
        return true;
    }

    bool scan(TSLexer *lexer, const bool *valid_symbols) {
        // A normal tree-sitter rule decided that the current branch is invalid and now "requests"
        // an error to stop the branch
        if (valid_symbols[TRIGGER_ERROR]) {
            return error(lexer);
        }

        // If we already matched all currently open blocks and just parsed a
        // `$._paragraph_end_newline` leave the matching state.
        uint8_t split_token_count = (state & STATE_SPLIT_TOKEN_COUNT) >> 2;
        if (
            split_token_count == 2 &&
            !valid_symbols[SOFT_LINE_BREAK_MARKER] &&
            matched == open_blocks.size()
            ) {
            state &= ~STATE_MATCHING;
        }

        // The parser just encountered a line break. Setup the state correspondingly
        if (valid_symbols[LINE_ENDING]) {
            // If the last line break ended a paragraph and no new block opened, the last line
            // break should have been a soft line break
            if (state & STATE_NEED_OPEN_BLOCK) return error(lexer);
            // Reset the counter for matched blocks
            matched = 0;
            // If there is at least one open block, we should be in the matching state.
            // Also set the matching flag if a `$._soft_line_break_marker` can be emitted so it
            // does get emitted.
            if (valid_symbols[SOFT_LINE_BREAK_MARKER] || open_blocks.size() > 0) {
                state |= STATE_MATCHING;
            } else {
                state &= (~STATE_MATCHING);
            }
            // reset some state variables
            state &=
            (~STATE_WAS_SOFT_LINE_BREAK) &
            (~STATE_SPLIT_TOKEN_COUNT) &
            (~STATE_NEED_OPEN_BLOCK) &
            (~STATE_JUST_CLOSED);
            indentation = 0;
            column = 0;
            lexer->result_symbol = LINE_ENDING;
            return true;
        }

        // Open a new (anonymous) block as requested. See `$._open_block` in grammar.js
        if (valid_symbols[OPEN_BLOCK] || valid_symbols[OPEN_BLOCK_DONT_INTERRUPT_PARAGRAPH]) {
            if (state & STATE_WAS_SOFT_LINE_BREAK) return error(lexer);
            if (valid_symbols[OPEN_BLOCK]) state &= ~STATE_NEED_OPEN_BLOCK;
            open_blocks.push_back(ANONYMOUS);
            lexer->result_symbol =
                valid_symbols[OPEN_BLOCK] ? OPEN_BLOCK : OPEN_BLOCK_DONT_INTERRUPT_PARAGRAPH;
            return true;
        }

        // Close the inner most block after the next line break as requested. See `$._close_block`
        // in grammar.js
        if (valid_symbols[CLOSE_BLOCK]) {
            state |= STATE_CLOSE_BLOCK;
            lexer->result_symbol = CLOSE_BLOCK;
            return true;
        }

        // Parse any preceeding whitespace and remember its length. This makes a lot of parsing
        // quite a bit easier.
        for (;;) {
            if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                indentation += advance(lexer);
            } else {
                break;
            }
        }

        // if we are at the end of the file and there are still open blocks close them all
        if (lexer->eof(lexer)) {
            if (open_blocks.size() > 0) {
                Block block = open_blocks[open_blocks.size() - 1];
                lexer->result_symbol = BLOCK_CLOSE;
                open_blocks.pop_back();
                return true;
            }
            return false;
        }

        if (!(state & STATE_MATCHING)) {
            // We are not matching. This is where the parsing logic for most "normal" token is.
            // Most importantly parsing logic for the start of new blocks.
            if (valid_symbols[INDENTED_CHUNK_START] && !valid_symbols[NO_INDENTED_CHUNK]) {
                if (indentation >= 4 && lexer->lookahead != '\n' && lexer->lookahead != '\r') {
                    lexer->result_symbol = INDENTED_CHUNK_START;
                    open_blocks.push_back(INDENTED_CODE_BLOCK);
                    indentation -= 4;
                    return true;
                }
            }
            // Decide which tokens to consider based on the first non-whitespace character
            switch (lexer->lookahead) {
                case '\r':
                case '\n':
                    if (valid_symbols[BLANK_LINE_START]) {
                        // A blank line token is actually just 0 width, so do not consume the
                        // characters
                        if (state & STATE_WAS_SOFT_LINE_BREAK) return error(lexer);
                        state &= ~STATE_NEED_OPEN_BLOCK;
                        lexer->result_symbol = BLANK_LINE_START;
                        return true;
                    }
                    break;
                case '`':
                    // A backtick could mark the beginning or ending of a fenced code block.
                    return parse_fenced_code_block('`', lexer, valid_symbols);
                    break;
                case '~':
                    // A tilde could mark the beginning or ending of a fenced code block.
                    return parse_fenced_code_block('~', lexer, valid_symbols);
                    break;
                case '*':
                    // A star could either mark  a list item or a thematic break.
                    // This code is similar to the code for '_' and '+'.
                    return parse_star(lexer, valid_symbols);
                    break;
                case '_':
                    return parse_thematic_break_underscore(lexer, valid_symbols);
                    break;
                case '>':
                    // A '>' could mark the beginning of a block quote
                    return parse_block_quote(lexer, valid_symbols);
                    break;
                case '#':
                    // A '#' could mark a atx heading
                    return parse_atx_heading(lexer, valid_symbols);
                    break;
                case '=':
                    // A '=' could mark a setext underline
                    return parse_setext_underline(lexer, valid_symbols);
                    break;
                case '+':
                    // A '+' could be a list marker
                    return parse_list_marker_plus(lexer, valid_symbols);
                    break;
                case '0':
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                case '8':
                case '9':
                    // A number could be a list marker (if followed by a dot or a parenthesis)
                    return parse_ordered_list_marker(lexer, valid_symbols);
                    break;
                case '-':
                    // A minus could mark a list marker, a thematic break or a setext underline
                    return parse_minus(lexer, valid_symbols);
                    break;
                case '<':
                    // A < could mark the beginning of a html block
                    return parse_html_block(lexer, valid_symbols);
                    break;
            }
        } else { // we are in the state of trying to match all currently open blocks
            bool partial_success = false;
            while (matched < open_blocks.size()) {
                if (matched == open_blocks.size() - 1 && (state & STATE_CLOSE_BLOCK)) {
                    if (!partial_success) state &= ~STATE_CLOSE_BLOCK;
                    break;
                }
                // If next block is a block quote and we have already matched stuff then return as
                // every continuation for block quotes should be its own token.
                if (open_blocks[matched] == BLOCK_QUOTE && partial_success) {
                    break;
                }
                if (match(lexer, open_blocks[matched])) {
                    partial_success = true;
                    matched++;
                    // Return after every block quote continuation
                    if (open_blocks[matched - 1] == BLOCK_QUOTE) {
                        break;
                    }
                } else {
                    break;
                }
            }
            if (partial_success) {
                if (!valid_symbols[SOFT_LINE_BREAK_MARKER] && matched == open_blocks.size()) {
                    state &= (~STATE_MATCHING);
                }
                if (open_blocks[matched - 1] == BLOCK_QUOTE) {
                    lexer->result_symbol = BLOCK_QUOTE_CONTINUATION;
                } else {
                    lexer->result_symbol = BLOCK_CONTINUATION;
                }
                return true;
            }

            uint8_t split_token_count = (state & STATE_SPLIT_TOKEN_COUNT) >> 2;
            if (valid_symbols[SPLIT_TOKEN] && split_token_count < 2) {
                split_token_count++;
                state &= ~STATE_SPLIT_TOKEN_COUNT;
                state |= split_token_count << 2;
                state |= STATE_NEED_OPEN_BLOCK;
                lexer->result_symbol = SPLIT_TOKEN;
                return true;
            }
            if (!valid_symbols[SOFT_LINE_BREAK_MARKER]) {
                Block block = open_blocks[open_blocks.size() - 1];
                lexer->result_symbol = BLOCK_CLOSE;
                if (block == FENCED_CODE_BLOCK) {
                    lexer->mark_end(lexer);
                    indentation = 0;
                }
                open_blocks.pop_back();
                if (matched == open_blocks.size()) {
                    state &= (~STATE_MATCHING);
                }
                state |= STATE_JUST_CLOSED;
                return true;
            } else {
                state &= (~STATE_MATCHING) & (~STATE_NEED_OPEN_BLOCK);
                state |= STATE_WAS_SOFT_LINE_BREAK;
                lexer->result_symbol = SOFT_LINE_BREAK_MARKER;
                return true;
            }
        }
        return false;
    }

    // Try to match the given block, i.e. consume all tokens that belong to the block. These are
    // 1. indentation for list items and indented code blocks
    // 2. '>' for block quotes
    // Returns true if the block is matched and false otherwise
    bool match(TSLexer *lexer, Block block) {
        switch (block) {
            case INDENTED_CODE_BLOCK:
                if (indentation >= 4 && lexer->lookahead != '\n' && lexer->lookahead != '\r') {
                    indentation -= 4;
                    return true;
                }
                break;
            case LIST_ITEM:
            case LIST_ITEM + 1:
            case LIST_ITEM + 2:
            case LIST_ITEM + 3:
            case LIST_ITEM + 4:
            case LIST_ITEM + 5:
            case LIST_ITEM + 6:
            case LIST_ITEM + 7:
            case LIST_ITEM + 8:
            case LIST_ITEM + 9:
            case LIST_ITEM + 10:
            case LIST_ITEM + 11:
            case LIST_ITEM + 12:
            case LIST_ITEM + 13:
            case LIST_ITEM + 14:
            case LIST_ITEM + 15:
                if (indentation >= list_item_indentation(open_blocks[matched])) {
                    indentation -= list_item_indentation(open_blocks[matched]);
                    return true;
                }
                if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
                    indentation = 0;
                    return true;
                }
                break;
            case BLOCK_QUOTE:
                if (lexer->lookahead == '>') {
                    advance(lexer);
                    indentation = 0;
                    if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                        indentation += advance(lexer) - 1;
                    }
                    return true;
                }
                break;
            case FENCED_CODE_BLOCK:
            case ANONYMOUS:
                return true;
        }
        return false;
    }

    bool parse_fenced_code_block(const char delimiter, TSLexer *lexer, const bool *valid_symbols) {
        // count the number of backticks
        size_t level = 0;
        while (lexer->lookahead == delimiter) {
            advance(lexer);
            level++;
        }
        lexer->mark_end(lexer);
        // If this is able to close a fenced code block then that is the only valid interpretation.
        // It can only close a fenced code block if the number of backticks is at least the number
        // of backticks of the opening delimiter. Also it cannot be indented more than 3 spaces.
        if (
            (delimiter == '`' ?
                valid_symbols[FENCED_CODE_BLOCK_END_BACKTICK] :
                valid_symbols[FENCED_CODE_BLOCK_END_TILDE]) &&
            indentation < 4 &&
            level >= fenced_code_block_delimiter_length &&
            (lexer->lookahead == '\n' || lexer->lookahead == '\r')
            ) {
            lexer->result_symbol = delimiter == '`' ?
                FENCED_CODE_BLOCK_END_BACKTICK :
                FENCED_CODE_BLOCK_END_TILDE;
            return true;
        }
        // If this could be the start of a fenced code block, check if the info string contains any
        // backticks.
        if ((delimiter == '`' ?
            valid_symbols[FENCED_CODE_BLOCK_START_BACKTICK] :
            valid_symbols[FENCED_CODE_BLOCK_START_TILDE]) && level >= 3) {
            bool info_string_has_backtick = false;
            if (delimiter == '`') {
                while (lexer->lookahead != '\n' && lexer->lookahead != '\r' && !lexer->eof(lexer)) {
                    if (lexer->lookahead == '`') {
                        info_string_has_backtick = true;
                        break;
                    }
                    advance(lexer);
                }
            }
            // If it does not then choose to interpret this as the start of a fenced code block.
            if (!info_string_has_backtick) {
                lexer->result_symbol = delimiter == '`' ?
                    FENCED_CODE_BLOCK_START_BACKTICK :
                    FENCED_CODE_BLOCK_START_TILDE;
                if (state & STATE_WAS_SOFT_LINE_BREAK) return error(lexer);
                state &= ~STATE_NEED_OPEN_BLOCK;
                open_blocks.push_back(FENCED_CODE_BLOCK);
                // Remember the length of the delimiter for later, since we need it to decide
                // whether a sequence of backticks can close the block.
                fenced_code_block_delimiter_length = level;
                indentation = 0;
                return true;
            }
        }
        return false;
    }

    bool parse_star(TSLexer *lexer, const bool *valid_symbols) {
        advance(lexer);
        lexer->mark_end(lexer);
        // Otherwise count the number of stars permitting whitespaces between them.
        size_t star_count = 1;
        // Also remember how many stars there are before the first whitespace...
        bool had_whitespace = false;
        size_t star_count_before_whitespace = 1;
        // ...and how many spaces follow the first star.
        size_t extra_indentation = 0;
        for (;;) {
            if (lexer->lookahead == '*') {
                if (star_count == 1 && extra_indentation >= 1 && valid_symbols[LIST_MARKER_STAR]) {
                    // If we get to this point then the token has to be at least this long. We need
                    // to call `mark_end` here in case we decide later that this is a list item.
                    lexer->mark_end(lexer);
                }
                if (!had_whitespace) {
                    star_count_before_whitespace++;
                }
                star_count++;
                advance(lexer);
            } else if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                had_whitespace = true;
                if (star_count == 1) {
                    extra_indentation += advance(lexer);
                } else {
                    advance(lexer);
                }
            } else {
                break;
            }
        }
        bool line_end = lexer->lookahead == '\n' || lexer->lookahead == '\r';
        bool dont_interrupt = false;
        if (star_count == 1 && line_end) {
            extra_indentation = 1;
            // line is empty so don't interrupt paragraphs if this is a list marker
            dont_interrupt = !(state & STATE_JUST_CLOSED) && matched == open_blocks.size();
        }
        // If there were at least 3 stars then this could be a thematic break
        bool thematic_break = star_count >= 3 && line_end;
        // If there was a star and at least one space after that star then this could be a list
        // marker.
        bool list_marker_star = star_count >= 1 && extra_indentation >= 1;
        if (valid_symbols[THEMATIC_BREAK] && thematic_break && indentation < 4) {
            // If a thematic break is valid then it takes precedence
            if (state & STATE_WAS_SOFT_LINE_BREAK) return error(lexer);
            state &= ~STATE_NEED_OPEN_BLOCK;
            lexer->result_symbol = THEMATIC_BREAK;
            lexer->mark_end(lexer);
            indentation = 0;
            return true;
        } else if (
            (dont_interrupt ?
                valid_symbols[LIST_MARKER_STAR_DONT_INTERRUPT] :
                valid_symbols[LIST_MARKER_STAR])
            && list_marker_star
            ) {
            // List markers take precedence over emphasis markers
            if (state & STATE_WAS_SOFT_LINE_BREAK) return error(lexer);
            if (!dont_interrupt) state &= ~STATE_NEED_OPEN_BLOCK;
            if (dont_interrupt && (state & STATE_NEED_OPEN_BLOCK)) return error(lexer);
            // If star_count > 1 then we already called mark_end at the right point. Otherwise the
            // token should go until this point.
            if (star_count == 1) {
                lexer->mark_end(lexer);
            }
            // Not counting one space...
            extra_indentation--;
            // ... check if the list item begins with an indented code block
            if (extra_indentation <= 3) {
                // If not then calculate the indentation level of the list item content as 
                // indentation of list marker + indentation after list marker - 1
                extra_indentation += indentation;
                indentation = 0;
            } else {
                // Otherwise the indentation level is just the indentation of the list marker. We
                // keep the indentation after the list marker for later blocks.
                size_t temp = indentation;
                indentation = extra_indentation;
                extra_indentation = temp;
            }
            open_blocks.push_back(Block(LIST_ITEM + extra_indentation));
            lexer->result_symbol =
                dont_interrupt ? LIST_MARKER_STAR_DONT_INTERRUPT : LIST_MARKER_STAR;
            return true;
        }
        return false;
    }


    bool parse_thematic_break_underscore(TSLexer *lexer, const bool *valid_symbols) {
        advance(lexer);
        lexer->mark_end(lexer);
        size_t underscore_count = 1;
        for (;;) {
            if (lexer->lookahead == '_') {
                underscore_count++;
                advance(lexer);
            } else if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                advance(lexer);
            } else {
                break;
            }
        }
        bool line_end = lexer->lookahead == '\n' || lexer->lookahead == '\r';
        if (underscore_count >= 3 && line_end && valid_symbols[THEMATIC_BREAK]) {
            if (state & STATE_WAS_SOFT_LINE_BREAK) return error(lexer);
            state &= ~STATE_NEED_OPEN_BLOCK;
            lexer->result_symbol = THEMATIC_BREAK;
            lexer->mark_end(lexer);
            indentation = 0;
            return true;
        }
        return false;
    }

    bool parse_block_quote(TSLexer *lexer, const bool *valid_symbols) {
        if (valid_symbols[BLOCK_QUOTE_START]) {
            if (state & STATE_WAS_SOFT_LINE_BREAK) return error(lexer);
            state &= ~STATE_NEED_OPEN_BLOCK;
            advance(lexer);
            indentation = 0;
            if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                indentation += advance(lexer) - 1;
            }
            lexer->result_symbol = BLOCK_QUOTE_START;
            open_blocks.push_back(BLOCK_QUOTE);
            return true;
        }
        return false;
    }

    bool parse_atx_heading(TSLexer *lexer, const bool *valid_symbols) {
        if (valid_symbols[ATX_H1_MARKER] && indentation <= 3) {
            lexer->mark_end(lexer);
            size_t level = 0;
            while (lexer->lookahead == '#' && level <= 6) {
                advance(lexer);
                level++;
            }
            if (level <= 6 && (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\n' || lexer->lookahead == '\r')) {
                if (state & STATE_WAS_SOFT_LINE_BREAK) return error(lexer);
                state &= ~STATE_NEED_OPEN_BLOCK;
                lexer->result_symbol = ATX_H1_MARKER + (level - 1);
                indentation = 0;
                lexer->mark_end(lexer);
                return true;
            }
        }
        return false;
    }

    bool parse_setext_underline(TSLexer *lexer, const bool *valid_symbols) {
        if (valid_symbols[SETEXT_H1_UNDERLINE] && matched == open_blocks.size()) {
            lexer->mark_end(lexer);
            while (lexer->lookahead == '=') {
                advance(lexer);
            }
            while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                advance(lexer);
            }
            if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
                if (state & STATE_WAS_SOFT_LINE_BREAK) return error(lexer);
                state &= ~STATE_NEED_OPEN_BLOCK;
                lexer->result_symbol = SETEXT_H1_UNDERLINE;
                lexer->mark_end(lexer);
                return true;
            }
        }
        return false;
    }

    bool parse_list_marker_plus(TSLexer *lexer, const bool *valid_symbols) {
        if (indentation <= 3 && (valid_symbols[LIST_MARKER_PLUS] || valid_symbols[LIST_MARKER_PLUS_DONT_INTERRUPT])) {
            advance(lexer);
            size_t extra_indentation = 0;
            while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                extra_indentation += advance(lexer);
            }
            bool dont_interrupt = false;
            if (lexer->lookahead == '\r' || lexer->lookahead == '\n') {
                extra_indentation = 1;
                dont_interrupt = true;
            }
            dont_interrupt = dont_interrupt && !(state & STATE_JUST_CLOSED) && matched == open_blocks.size();
            if (extra_indentation >= 1 && (dont_interrupt ? valid_symbols[LIST_MARKER_PLUS_DONT_INTERRUPT] : valid_symbols[LIST_MARKER_PLUS])) {
                if (state & STATE_WAS_SOFT_LINE_BREAK) return error(lexer);
                if (!dont_interrupt) state &= ~STATE_NEED_OPEN_BLOCK;
                if (dont_interrupt && (state & STATE_NEED_OPEN_BLOCK)) return error(lexer);
                lexer->result_symbol = dont_interrupt ? LIST_MARKER_PLUS_DONT_INTERRUPT : LIST_MARKER_PLUS;
                extra_indentation--;
                if (extra_indentation <= 3) {
                    extra_indentation += indentation;
                    indentation = 0;
                } else {
                    size_t temp = indentation;
                    indentation = extra_indentation;
                    extra_indentation = temp;
                }
                open_blocks.push_back(Block(LIST_ITEM + extra_indentation));
                return true;
            }
        }
        return false;
    }

    bool parse_ordered_list_marker(TSLexer *lexer, const bool *valid_symbols) {
        if (indentation <= 3 && (valid_symbols[LIST_MARKER_PARENTHESIS] || valid_symbols[LIST_MARKER_DOT])) {
            size_t digits = 1;
            bool dont_interrupt = lexer->lookahead != '1';
            advance(lexer);
            while (lexer->lookahead >= '0' && lexer->lookahead <= '9') {
                dont_interrupt = true;
                digits++;
                advance(lexer);
            }
            if (digits >= 1 && digits <= 9) {
                bool success = false;
                bool dot = false;
                bool parenthesis = false;
                if (lexer->lookahead == '.') {
                    advance(lexer);
                    dot = true;
                } else if (lexer->lookahead == ')') {
                    advance(lexer);
                    parenthesis = true;
                }
                if (dot || parenthesis) {
                    size_t extra_indentation = 0;
                    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                        extra_indentation += advance(lexer);
                    }
                    bool line_end = lexer->lookahead == '\n' || lexer->lookahead == '\r';
                    if (line_end) {
                        extra_indentation = 1;
                        dont_interrupt = true;
                    }
                    dont_interrupt = dont_interrupt && !(state & STATE_JUST_CLOSED) && matched == open_blocks.size();
                    if (extra_indentation >= 1 && (dot ? (dont_interrupt ? valid_symbols[LIST_MARKER_DOT_DONT_INTERRUPT] : valid_symbols[LIST_MARKER_DOT]) : (dont_interrupt ? valid_symbols[LIST_MARKER_PARENTHESIS_DONT_INTERRUPT] : valid_symbols[LIST_MARKER_PARENTHESIS]))) {
                        if (state & STATE_WAS_SOFT_LINE_BREAK) return error(lexer);
                        if (!dont_interrupt) state &= ~STATE_NEED_OPEN_BLOCK;
                        if (dont_interrupt && (state & STATE_NEED_OPEN_BLOCK)) return error(lexer);
                        lexer->result_symbol = dot ? LIST_MARKER_DOT : LIST_MARKER_PARENTHESIS;
                        extra_indentation--;
                        if (extra_indentation <= 3) {
                            extra_indentation += indentation;
                            indentation = 0;
                        } else {
                            size_t temp = indentation;
                            indentation = extra_indentation;
                            extra_indentation = temp;
                        }
                        open_blocks.push_back(Block(LIST_ITEM + extra_indentation + digits));
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool parse_minus(TSLexer *lexer, const bool *valid_symbols) {
        if (indentation <= 3 && (valid_symbols[LIST_MARKER_MINUS] || valid_symbols[LIST_MARKER_MINUS_DONT_INTERRUPT] || valid_symbols[SETEXT_H2_UNDERLINE] || valid_symbols[THEMATIC_BREAK])) {
            lexer->mark_end(lexer);
            bool whitespace_after_minus = false;
            bool minus_after_whitespace = false;
            size_t minus_count = 0;
            size_t extra_indentation = 0;

            for (;;) {
                if (lexer->lookahead == '-') {
                    if (minus_count == 1 && extra_indentation >= 1) {
                        lexer->mark_end(lexer);
                    }
                    minus_count++;
                    advance(lexer);
                    minus_after_whitespace = whitespace_after_minus;
                } else if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                    if (minus_count == 1) {
                        extra_indentation += advance(lexer);
                    } else {
                        advance(lexer);
                    }
                    whitespace_after_minus = true;
                } else {
                    break;
                }
            }
            bool line_end = lexer->lookahead == '\n' || lexer->lookahead == '\r';
            bool dont_interrupt = false;
            if (minus_count == 1 && line_end) {
                extra_indentation = 1;
                dont_interrupt = true;
            }
            dont_interrupt = dont_interrupt && !(state & STATE_JUST_CLOSED) && matched == open_blocks.size();
            bool thematic_break = minus_count >= 3 && line_end;
            bool underline = minus_count >= 1 && !minus_after_whitespace && line_end && matched == open_blocks.size(); // setext heading can not break lazy continuation
            bool list_marker_minus = minus_count >= 1 && extra_indentation >= 1;
            if (valid_symbols[SETEXT_H2_UNDERLINE] && underline) {
                if (state & STATE_WAS_SOFT_LINE_BREAK) return error(lexer);
                state &= ~STATE_NEED_OPEN_BLOCK;
                lexer->result_symbol = SETEXT_H2_UNDERLINE;
                lexer->mark_end(lexer);
                indentation = 0;
                return true;
            } else if (valid_symbols[THEMATIC_BREAK] && thematic_break) { // underline is false if list_marker_minus is true
                if (state & STATE_WAS_SOFT_LINE_BREAK) return error(lexer);
                state &= ~STATE_NEED_OPEN_BLOCK;
                lexer->result_symbol = THEMATIC_BREAK;
                lexer->mark_end(lexer);
                indentation = 0;
                return true;
            } else if ((dont_interrupt ? valid_symbols[LIST_MARKER_MINUS_DONT_INTERRUPT] : valid_symbols[LIST_MARKER_MINUS]) && list_marker_minus) {
                if (state & STATE_WAS_SOFT_LINE_BREAK) return error(lexer);
                if (!dont_interrupt) state &= ~STATE_NEED_OPEN_BLOCK;
                if (dont_interrupt && (state & STATE_NEED_OPEN_BLOCK)) return error(lexer);
                if (minus_count == 1) {
                    lexer->mark_end(lexer);
                }
                extra_indentation--;
                if (extra_indentation <= 3) {
                    extra_indentation += indentation;
                    indentation = 0;
                } else {
                    size_t temp = indentation;
                    indentation = extra_indentation;
                    extra_indentation = temp;
                }
                open_blocks.push_back(Block(LIST_ITEM + extra_indentation));
                lexer->result_symbol = dont_interrupt ? LIST_MARKER_MINUS_DONT_INTERRUPT : LIST_MARKER_MINUS;
                return true;
            }
        }
        return false;
    }

    bool parse_html_block(TSLexer *lexer, const bool *valid_symbols) {
        if (!(valid_symbols[HTML_BLOCK_1_START] || valid_symbols[HTML_BLOCK_1_END] ||
            valid_symbols[HTML_BLOCK_2_START] || valid_symbols[HTML_BLOCK_3_START] ||
            valid_symbols[HTML_BLOCK_4_START] || valid_symbols[HTML_BLOCK_5_START] ||
            valid_symbols[HTML_BLOCK_6_START] || valid_symbols[HTML_BLOCK_7_START])) {
            return false;
        }
        advance(lexer);
        if (lexer->lookahead == '?' && valid_symbols[HTML_BLOCK_3_START]) {
            advance(lexer);
            lexer->result_symbol = HTML_BLOCK_3_START;
            return true;
        }
        if (lexer->lookahead == '!') {
            // could be block 2
            advance(lexer);
            if (lexer->lookahead == '-') {
                advance(lexer);
                if (lexer->lookahead == '-' && valid_symbols[HTML_BLOCK_2_START]) {
                    advance(lexer);
                    lexer->result_symbol = HTML_BLOCK_2_START;
                    return true;
                }
            } else if ('A' <= lexer->lookahead && lexer->lookahead <= 'Z' && valid_symbols[HTML_BLOCK_4_START]) {
                advance(lexer);
                lexer->result_symbol = HTML_BLOCK_4_START;
                return true;
            } else if (lexer->lookahead == '[') {
                advance(lexer);
                if (lexer->lookahead == 'C') {
                advance(lexer);
                if (lexer->lookahead == 'D') {
                advance(lexer);
                if (lexer->lookahead == 'A') {
                advance(lexer);
                if (lexer->lookahead == 'T') {
                advance(lexer);
                if (lexer->lookahead == 'A') {
                advance(lexer);
                if (lexer->lookahead == '[' && valid_symbols[HTML_BLOCK_5_START]) {
                    advance(lexer);
                    lexer->result_symbol = HTML_BLOCK_5_START;
                    return true;
                }}}}}}
            }
        }
        bool starting_slash = lexer->lookahead == '/';
        if (starting_slash) {
            advance(lexer);
        }
        char name[11];
        size_t name_length = 0;
        while (isalpha(lexer->lookahead)) {
            if (name_length < 10) {
                name[name_length++] = tolower(lexer->lookahead);
            } else {
                name_length = 12;
            }
            advance(lexer);
        }
        if (name_length == 0) {
            return false;
        }
        bool tag_closed = false;
        if (name_length < 11) {
            name[name_length] = 0;
            bool next_symbol_valid = 
                lexer->lookahead == ' ' || lexer->lookahead == '\t' ||
                lexer->lookahead == '\n' || lexer->lookahead == '\r' ||
                lexer->lookahead == '>';
            if (next_symbol_valid) {
                // try block 1 names
                for (size_t i = 0; i < NUM_HTML_TAG_NAMES_RULE_1; i++) {
                    if (strcmp(name, HTML_TAG_NAMES_RULE_1[i]) == 0) {
                        if (starting_slash) {
                            if (valid_symbols[HTML_BLOCK_1_END]) {
                                lexer->result_symbol = HTML_BLOCK_1_END;
                                return true;
                            }
                        } else if (valid_symbols[HTML_BLOCK_1_START]) {
                            lexer->result_symbol = HTML_BLOCK_1_START;
                            return true;
                        }
                    }
                }
            }
            if (!next_symbol_valid && lexer->lookahead == '/') {
                advance(lexer);
                if (lexer->lookahead == '>') {
                    advance(lexer);
                    tag_closed = true;
                }
            }
            if (next_symbol_valid || tag_closed) {
                // try block 2 names
                for (size_t i = 0; i < NUM_HTML_TAG_NAMES_RULE_7; i++) {
                    if (strcmp(name, HTML_TAG_NAMES_RULE_7[i]) == 0 && valid_symbols[HTML_BLOCK_6_START]) {
                        lexer->result_symbol = HTML_BLOCK_6_START;
                        return true;
                    }
                }
            }
        }

        if (!valid_symbols[HTML_BLOCK_7_START]) {
            return false;
        }
        
        if (!tag_closed) {
            // tag name (continued)
            while (isalnum(lexer->lookahead) || lexer->lookahead == '-') {
                advance(lexer);
            }
            if (!starting_slash) {
                // attributes
                bool had_whitespace = false;
                for (;;) {
                    // whitespace
                    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                        had_whitespace = true;
                        advance(lexer);
                    }
                    if (lexer->lookahead == '/') {
                        advance(lexer);
                        break;
                    }
                    if (lexer->lookahead == '>') {
                        break;
                    }
                    // attribute name
                    if (!had_whitespace) {
                        return false;
                    }
                    if (!isalpha(lexer->lookahead) && lexer->lookahead != '_' && lexer->lookahead != ':') {
                        return false;
                    }
                    had_whitespace = false;
                    advance(lexer);
                    while (isalnum(lexer->lookahead) || lexer->lookahead == '_' || lexer->lookahead == '.' || lexer->lookahead == ':' || lexer->lookahead == '-') {
                        advance(lexer);
                    }
                    // attribute value specification
                    // optional whitespace
                    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                        had_whitespace = true;
                        advance(lexer);
                    }
                    // =
                    if (lexer->lookahead == '=') {
                        advance(lexer);
                        had_whitespace = false;
                        // optional whitespace
                        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                            advance(lexer);
                        }
                        // attribute value
                        if (lexer->lookahead == '\'' || lexer->lookahead == '"') {
                            char delimiter = lexer->lookahead;
                            advance(lexer);
                            while (lexer->lookahead != delimiter && lexer->lookahead != '\n' && lexer->lookahead != '\r' && !lexer->eof(lexer)) {
                                advance(lexer);
                            }
                            if (lexer->lookahead != delimiter) {
                                return false;
                            }
                            advance(lexer);
                        } else {
                            // unquoted attribute value
                            bool had_one = false;
                            while (lexer->lookahead != ' ' && lexer->lookahead != '\t' && lexer->lookahead != '"' && lexer->lookahead != '\'' && lexer->lookahead != '=' && lexer->lookahead != '<' && lexer->lookahead != '>' && lexer->lookahead != '`' && lexer->lookahead != '\n' && lexer->lookahead != '\r' && !lexer->eof(lexer)) {
                                advance(lexer);
                                had_one = true;
                            }
                            if (!had_one) {
                                return false;
                            }
                        }
                    }
                }
            } else {
                while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                    advance(lexer);
                }
            }
            if (lexer->lookahead != '>') {
                return false;
            }
            advance(lexer);
        }
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            advance(lexer);
        }
        if (lexer->lookahead == '\r' || lexer->lookahead == '\n') {
            lexer->result_symbol = HTML_BLOCK_7_START;
            return true;
        }
        return false;
    }

};

}

extern "C" {
    void *tree_sitter_markdown_external_scanner_create() {
        return new TreeSitterMarkdown::Scanner();
    }

    bool tree_sitter_markdown_external_scanner_scan(
        void *payload,
        TSLexer *lexer,
        const bool *valid_symbols
    ) {
        TreeSitterMarkdown::Scanner *scanner = static_cast<TreeSitterMarkdown::Scanner *>(payload);
        return scanner->scan(lexer, valid_symbols);
    }

    unsigned tree_sitter_markdown_external_scanner_serialize(
        void *payload,
        char* buffer
    ) {
        TreeSitterMarkdown::Scanner *scanner = static_cast<TreeSitterMarkdown::Scanner *>(payload);
        return scanner->serialize(buffer);
    }

    void tree_sitter_markdown_external_scanner_deserialize(
        void *payload,
        char* buffer,
        unsigned length
    ) {
        TreeSitterMarkdown::Scanner *scanner = static_cast<TreeSitterMarkdown::Scanner *>(payload);
        scanner->deserialize(buffer, length);
    }

    void tree_sitter_markdown_external_scanner_destroy(void *payload) {
        TreeSitterMarkdown::Scanner *scanner = static_cast<TreeSitterMarkdown::Scanner *>(payload);
        delete scanner;
    }
}
