{
  description = "xv6-riscv - a teaching OS for RISC-V";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        riscv-pkgs = pkgs.pkgsCross.riscv64-embedded.buildPackages;
      in {
        devShells.default = pkgs.mkShell {
          nativeBuildInputs = with pkgs; [
            riscv-pkgs.gcc
            riscv-pkgs.binutils
            qemu
            gcc
            perl
            bc
          ];
        };

      });
}
