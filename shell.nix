{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
	name = "rat";

	packages = [
		pkgs.gcc
		pkgs.clang-tools
		(pkgs.python3.withPackages (ps: [ ps.matplotlib ]))
	];
}
