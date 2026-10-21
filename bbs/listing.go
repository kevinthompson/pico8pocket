// Package bbs reads public cartridge metadata from Lexaloffle's BBS lister.
package bbs

import (
	"fmt"
	"html"
	"net/url"
	"regexp"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf16"
	"unicode/utf8"
)

type Order string

const (
	Recent     Order = "recent"
	Popular    Order = "popular"
	MaxPage          = 1000
	MaxListing       = 1024 * 1024
	MaxSearch        = 120
)

type Cart struct{ ID, Title, Author string }

func (c Cart) URL() string { return "https://www.lexaloffle.com/bbs/?pid=" + c.ID }

type Page struct {
	Carts   []Cart
	HasNext bool
}

// NormalizeSearch bounds text supplied by the host before constructing a BBS URL.
func NormalizeSearch(query string) (string, error) {
	if !utf8.ValidString(query) || utf8.RuneCountInString(query) > MaxSearch {
		return "", fmt.Errorf("search must be at most %d characters", MaxSearch)
	}
	for _, c := range query {
		if unicode.IsControl(c) {
			return "", fmt.Errorf("search contains a control character")
		}
	}
	return strings.TrimSpace(query), nil
}

func ListingURL(order Order, page int, search string) (string, error) {
	if (order != Recent && order != Popular) || page < 1 || page > MaxPage {
		return "", fmt.Errorf("invalid BBS listing order or page")
	}
	normalized, err := NormalizeSearch(search)
	if err != nil || normalized != search || (search != "" && order != Recent) {
		return "", fmt.Errorf("invalid BBS search")
	}
	q := url.Values{"cat": {"7"}, "sub": {"2"}, "mode": {"carts"}, "orderby": {"ts"}, "page": {strconv.Itoa(page)}, "use_hurl": {"1"}}
	if search != "" {
		q.Set("search", search)
	}
	if order == Popular {
		q.Set("popular", "1")
	}
	return "https://www.lexaloffle.com/bbs/lister.php?" + q.Encode(), nil
}

var scriptStart = regexp.MustCompile(`(?i)<script\s+id\s*=\s*(?:"cart_data_script"|'cart_data_script'|cart_data_script)\s*>`)
var dataStart = regexp.MustCompile(`(?m)^\s*(?:var\s+)?pdat\s*=\s*\[`)
var href = regexp.MustCompile(`(?i)\bhref\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))`)

// Parse accepts the literal pdat array emitted by lister.php. It never evaluates
// JavaScript or follows page-supplied URLs. Expressions and schema changes fail closed.
func Parse(data []byte, page int) (Page, error) {
	var result Page
	if page < 1 || page > MaxPage || len(data) > MaxListing || !utf8.Valid(data) {
		return result, fmt.Errorf("invalid or oversized BBS listing")
	}
	source := string(data)
	start := scriptStart.FindStringIndex(source)
	if start == nil {
		return result, fmt.Errorf("BBS cart metadata missing")
	}
	script := source[start[1]:]
	end := strings.Index(strings.ToLower(script), "</script>")
	if end < 0 {
		return result, fmt.Errorf("BBS cart metadata truncated")
	}
	script = script[:end]
	at := dataStart.FindStringIndex(script)
	if at == nil {
		return result, fmt.Errorf("BBS cart array missing")
	}
	reader := literalReader{source: script[at[1]-1:]}
	value, err := reader.value(0)
	if err != nil {
		return result, fmt.Errorf("BBS metadata: %w", err)
	}
	if tail := strings.TrimSpace(reader.source[reader.pos:]); !strings.HasPrefix(tail, ";") {
		return result, fmt.Errorf("invalid BBS array terminator")
	}
	rows, ok := value.([]any)
	if !ok || len(rows) > 60 {
		return result, fmt.Errorf("invalid BBS cart array")
	}
	seen := map[string]bool{}
	for _, value := range rows {
		row, ok := value.([]any)
		if !ok || len(row) != 24 || row[15] != float64(7) || row[21] != float64(7) {
			return Page{}, fmt.Errorf("unexpected BBS cartridge schema or console")
		}
		id, ok := row[0].(string)
		if !ok || !numericID(id) || seen[id] {
			return Page{}, fmt.Errorf("invalid or duplicate BBS cart ID")
		}
		seen[id] = true
		title, ok := row[22].(string)
		if !ok {
			return Page{}, fmt.Errorf("missing BBS cart title")
		}
		if title == "" {
			title, _ = row[2].(string)
		}
		author, ok := row[8].(string)
		if !ok {
			return Page{}, fmt.Errorf("missing BBS cart author")
		}
		title, err = plainText(title, 200)
		if err != nil {
			return Page{}, err
		}
		author, err = plainText(author, 80)
		if err != nil {
			return Page{}, err
		}
		result.Carts = append(result.Carts, Cart{ID: id, Title: title, Author: author})
	}
	for _, match := range href.FindAllStringSubmatch(source, -1) {
		target := match[1] + match[2] + match[3]
		if !strings.HasPrefix(target, "#") && !strings.HasPrefix(target, "?") {
			continue
		}
		q, err := url.ParseQuery(html.UnescapeString(target[1:]))
		if err == nil && q.Get("mode") == "carts" && q.Get("page") == strconv.Itoa(page+1) && page < MaxPage {
			result.HasNext = true
		}
	}
	return result, nil
}
func numericID(id string) bool {
	if len(id) == 0 || len(id) > 20 || id[0] == '0' {
		return false
	}
	for _, c := range id {
		if c < '0' || c > '9' {
			return false
		}
	}
	return true
}
func plainText(value string, max int) (string, error) {
	value = html.UnescapeString(value)
	for _, c := range value {
		if unicode.IsControl(c) && !unicode.IsSpace(c) {
			return "", fmt.Errorf("control character in BBS metadata")
		}
	}
	value = strings.Join(strings.Fields(value), " ")
	if value == "" || utf8.RuneCountInString(value) > max {
		return "", fmt.Errorf("invalid BBS metadata text length")
	}
	return value, nil
}

// The BBS array uses JavaScript quoted strings, sparse fields and trailing commas.
// This bounded reader supports data literals only, without a JavaScript runtime.
type literalReader struct {
	source     string
	pos, nodes int
}

func (r *literalReader) space() {
	for r.pos < len(r.source) && strings.ContainsRune(" \t\r\n", rune(r.source[r.pos])) {
		r.pos++
	}
}
func (r *literalReader) value(depth int) (any, error) {
	r.space()
	r.nodes++
	if depth > 8 || r.nodes > 8192 || r.pos >= len(r.source) {
		return nil, fmt.Errorf("invalid metadata bounds")
	}
	c := r.source[r.pos]
	if c == '\'' || c == '"' || c == '`' {
		return r.string()
	}
	if c == '[' {
		r.pos++
		var values []any
		for {
			r.space()
			if r.pos >= len(r.source) {
				return nil, fmt.Errorf("unterminated array")
			}
			if r.source[r.pos] == ']' {
				r.pos++
				return values, nil
			}
			if r.source[r.pos] == ',' {
				r.nodes++
				if r.nodes > 8192 {
					return nil, fmt.Errorf("metadata value limit exceeded")
				}
				values = append(values, nil)
				r.pos++
				continue
			}
			v, err := r.value(depth + 1)
			if err != nil {
				return nil, err
			}
			values = append(values, v)
			r.space()
			if r.pos >= len(r.source) {
				return nil, fmt.Errorf("unterminated array")
			}
			if r.source[r.pos] == ']' {
				r.pos++
				return values, nil
			}
			if r.source[r.pos] != ',' {
				return nil, fmt.Errorf("expected array separator")
			}
			r.pos++
		}
	}
	start := r.pos
	for r.pos < len(r.source) && !strings.ContainsRune(",] \t\r\n", rune(r.source[r.pos])) {
		r.pos++
	}
	word := r.source[start:r.pos]
	if word == "null" {
		return nil, nil
	}
	number, err := strconv.ParseFloat(word, 64)
	if err != nil || word == "NaN" || strings.Contains(word, "Inf") {
		return nil, fmt.Errorf("nonliteral metadata value")
	}
	return number, nil
}
func (r *literalReader) string() (string, error) {
	quote := r.source[r.pos]
	r.pos++
	var out strings.Builder
	for r.pos < len(r.source) {
		c := r.source[r.pos]
		r.pos++
		if c == quote {
			return out.String(), nil
		}
		if quote == '`' && c == '$' && r.pos < len(r.source) && r.source[r.pos] == '{' {
			return "", fmt.Errorf("template expression in metadata")
		}
		if c != '\\' {
			out.WriteByte(c)
			continue
		}
		if r.pos >= len(r.source) {
			break
		}
		c = r.source[r.pos]
		r.pos++
		switch c {
		case '\\', '\'', '"', '`', '/':
			out.WriteByte(c)
		case 'n':
			out.WriteByte('\n')
		case 'r':
			out.WriteByte('\r')
		case 't':
			out.WriteByte('\t')
		case 'b':
			out.WriteByte('\b')
		case 'f':
			out.WriteByte('\f')
		case 'u':
			code, err := r.hexRune()
			if err != nil {
				return "", err
			}
			if utf16.IsSurrogate(code) {
				if code > 0xdbff || !strings.HasPrefix(r.source[r.pos:], `\u`) {
					return "", fmt.Errorf("unpaired Unicode surrogate")
				}
				r.pos += 2
				low, err := r.hexRune()
				if err != nil || low < 0xdc00 || low > 0xdfff {
					return "", fmt.Errorf("unpaired Unicode surrogate")
				}
				code = utf16.DecodeRune(code, low)
			}
			out.WriteRune(code)
		default:
			return "", fmt.Errorf("unsupported metadata string escape")
		}
	}
	return "", fmt.Errorf("unterminated metadata string")
}
func (r *literalReader) hexRune() (rune, error) {
	if len(r.source)-r.pos < 4 {
		return 0, fmt.Errorf("short Unicode escape")
	}
	n, err := strconv.ParseUint(r.source[r.pos:r.pos+4], 16, 16)
	r.pos += 4
	return rune(n), err
}
