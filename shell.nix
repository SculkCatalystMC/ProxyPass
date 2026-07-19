{ pkgs ? import <nixpkgs> {} }:

let
  opensslCombined = pkgs.symlinkJoin {
    name = "openssl-combined";
    paths = [ pkgs.openssl.out pkgs.openssl.dev ];
  };
in

pkgs.mkShell {
  buildInputs = with pkgs; [
    gcc15
    cmake
    ninja
    openssl
    git
    pkg-config
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
}
