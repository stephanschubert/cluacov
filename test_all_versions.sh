#!/bin/bash
# Test cluacov across multiple Lua versions.
# Requires uv: https://docs.astral.sh/uv/getting-started/installation/
#
# Usage: ./test_all_versions.sh [iterations]
#   iterations: number of benchmark iterations (default: 50000)

set -e

ITERATIONS=${1:-50000}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Lua versions matching upstream CI matrix
# Skip LuaJIT on ARM64 - cluacov's bundled LuaJIT headers don't support it
if [[ "$(uname -m)" == "arm64" ]]; then
    VERSIONS=("lua=5.1" "lua=5.2" "lua=5.3")
    echo "Note: Skipping LuaJIT on ARM64 (unsupported by cluacov's bundled headers)"
else
    VERSIONS=("lua=5.1" "lua=5.2" "lua=5.3" "luajit=2.1")
fi

# Check for uv (modern Python package manager)
if ! command -v uv &> /dev/null; then
    echo "Error: uv not found."
    echo "Install with: curl -LsSf https://astral.sh/uv/install.sh | sh"
    echo "Or visit: https://docs.astral.sh/uv/getting-started/installation/"
    exit 1
fi

for ver in "${VERSIONS[@]}"; do
    install_dir="lua_install_${ver//=/_}"
    echo ""
    echo "=========================================="
    echo "Testing with $ver"
    echo "=========================================="

    # Install Lua version if needed
    if [ ! -d "$install_dir" ]; then
        echo "Installing $ver..."
        uvx hererocks "$install_dir" --$ver -r latest
    fi

    # Activate environment
    source "$install_dir/bin/activate"

    # Install LuaCov from GitHub (upstream version)
    if ! lua -e "require 'luacov'" 2>/dev/null; then
        echo "Installing luacov..."
        # Install datafile dependency directly (avoids manifest parsing issues with LuaJIT)
        luarocks install https://luarocks.org/datafile-0.11-1.src.rock
        rm -rf /tmp/luacov_$$
        git clone --depth=1 https://github.com/keplerproject/luacov /tmp/luacov_$$
        (cd /tmp/luacov_$$ && luarocks make)
        rm -rf /tmp/luacov_$$
    fi

    # Install busted if needed
    if ! command -v busted &> /dev/null; then
        echo "Installing busted..."
        luarocks install busted
    fi

    # Build cluacov
    echo "Building cluacov..."
    luarocks make --deps-mode=none

    # Run tests
    echo "Running tests..."
    busted

    # Run tests with coverage
    echo "Running tests with coverage..."
    busted --coverage

    # Run benchmark
    echo "Running benchmark ($ITERATIONS iterations)..."
    lua benchmark/bench_hook.lua "$ITERATIONS"

    # Check tls_available
    tls=$(lua -e "local h = require 'cluacov.hook'; print(h.tls_available and 'yes' or 'no')")
    echo "TLS available: $tls"

    deactivate-lua

    echo "$ver: PASSED (TLS: $tls)"
done

echo ""
echo "=========================================="
echo "All versions tested successfully!"
echo "=========================================="
