# Magnificent

A lean, native C++ 3D animation workstation designed for mobile and 2-in-1 hardware, built around an OpenUSD backbone.

> **Status:** Early development (`v0.1` target: December 31, 2026). Expect active changes and rough edges as core systems stabilize.

---

## Key Goals & Architecture
* **Pure Native C++:** Zero C# bloat and minimal external dependencies for direct memory control and long-term portability.
* **Mobile & 2-in-1 Friendly:** Engineered with thermal and memory limits in mind, including a tile-aware renderer.
* **121-Slot Undo/Redo:** Bounded ring-buffer history for predictable memory usage during animation sessions.
* **OpenUSD Backbone:** Built around Universal Scene Description for clean, non-destructive scene structure.
* **Decoupled Core:** Core animation logic, math, and scene data are separated from UI/platform layers so the foundation can scale across decades—and eventually power a shared real-time C++ game engine.

---

## Roadmap

### `v0.1` (Target: December 31, 2026) — Core Stability
- [ ] Core scene & keyframe animation playback
- [ ] 121-slot undo/redo ring buffer
- [ ] Versioned project Save/Load
- [ ] Tile-aware renderer & viewport stability

### `v0.2` (Long-Term) — Pipeline Expansion
- [ ] Source Filmmaker `.dmx` session/animation import (not yet)
- [ ] Auto lip-sync tools
- [ ] Custom particle system (not yet)
- [ ] Expanded graph editor & curve tooling

---

## Building from Source

### Prerequisites
* A C++17 (or C++20) compatible compiler (`clang++`, `g++`, or MSVC)
* *(List any specific libraries or build tools you use here, e.g., CMake, SDL2, OpenGL/Vulkan, OpenUSD)*

### Build Steps
```bash
git clone [https://github.com/YOUR_USERNAME/Magnificent.git](https://github.com/YOUR_USERNAME/Magnificent.git)
cd Magnificent
# Replace with your actual compile/build command, for example:
# mkdir build && cd build && cmake .. && cmake --build .
