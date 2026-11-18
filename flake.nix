{
  description = "cnf grammar for tree-sitter";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            # Keep in step with the tree-sitter-ref pinned in
            # .github/workflows/ci.yml: the generated parser is checked
            # against that version.
            tree-sitter
            nodejs

            # Bindings; mkShell already provides the C toolchain.
            go_1_27
            cargo
            rustc
            python3
            gnumake
          ];
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt-rfc-style);
    };
}
