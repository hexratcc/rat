{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
	name = "rat";

	packages = [
		pkgs.gcc
		pkgs.gnumake
		pkgs.clang-tools
	];
}
