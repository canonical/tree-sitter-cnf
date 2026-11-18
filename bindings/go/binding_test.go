package tree_sitter_cnf_test

import (
	"testing"

	tree_sitter "github.com/tree-sitter/go-tree-sitter"
	tree_sitter_cnf "github.com/canonical/tree-sitter-cnf/bindings/go"
)

func TestCanLoadGrammar(t *testing.T) {
	language := tree_sitter.NewLanguage(tree_sitter_cnf.Language())
	if language == nil {
		t.Errorf("Error loading cnf grammar")
	}
}
