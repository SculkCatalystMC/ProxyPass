{
  description = "ProxyPass build environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      opensslStatic = pkgs.openssl.override { static = true; };
      opensslCombined = pkgs.symlinkJoin {
        name = "openssl-combined";
        paths = [ opensslStatic.out opensslStatic.dev ];
      };
    in
    {
      devShells.${system}.default = pkgs.mkShell {
        buildInputs = with pkgs; [
          gcc15
          cmake
          ninja
          openssl
          git
          pkg-config
          glibc.static 
        ];

        OPENSSL_ROOT_DIR = "${opensslCombined}";

        shellHook = ''
          echo "ProxyPass build environment"
          echo "GCC: $(gcc --version | head -n1)"
          echo "CMake: $(cmake --version | head -n1)"
          echo "Ninja: $(ninja --version)"
          echo ""
          echo "Build with:"
          echo "  CC=gcc CXX=g++ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release"
          echo "  cmake --build build --config Release --parallel"
        '';
      };
    };
}
