# clirasterizer

![tex](screenshots/Snipaste_2025-12-09_17-45-48.png)

A simple soft rasterizer in cli.

# opt

```
Rasterizer - Two-phase tile-based rendering (similar to PS5 NGGP)
┌─────────────────────────────────────────────────────────────────────┐
│                    Phase 1: Per-Triangle Parallel                   │
├─────────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐              │
│  │ Triangle 0  │    │ Triangle 1  │    │ Triangle N  │    ...       │
│  └──────┬──────┘    └──────┬──────┘    └──────┬──────┘              │
│         │                  │                  │                     │
│         ▼                  ▼                  ▼                     │
│  ┌──────────────────────────────────────────────────────┐           │
│  │              process_triangle()                      │           │
│  │  - Clip space → Screen space transform               │           │
│  │  - Frustum culling (behind camera)                   │           │
│  │  - Screen bounds culling                             │           │
│  │  - Degenerate triangle culling (zero area)           │           │
│  │  - Small triangle culling (sub-pixel)                │           │
│  │  - Compute tile range                                │           │
│  └──────────────────────────────────────────────────────┘           │
│         │                  │                  │                     │
│         ▼                  ▼                  ▼                     │
│  ┌──────────────────────────────────────────────────────┐           │
│  │              bin_triangle()                          │           │
│  │  - Add triangle index to covered tiles               │           │
│  │  - Thread-safe (mutex per tile)                      │           │
│  └──────────────────────────────────────────────────────┘           │
└─────────────────────────────────────────────────────────────────────┘
│
▼
┌─────────────────────────────────────────────────────────────────────┐
│                    Phase 2: Per-Tile Parallel                       │
├─────────────────────────────────────────────────────────────────────┤
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐             │
│  │  Tile 0  │  │  Tile 1  │  │  Tile 2  │  │  Tile N  │   ...       │
│  │ [T3,T7]  │  │ [T1,T3]  │  │   [T7]   │  │[T1,T3,T7]│             │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘             │
│       │             │             │             │                   │
│       ▼             ▼             ▼             ▼                   │
│  Only rasterize triangles in each tile's bin list!                  │
│  (No full triangle list traversal needed)                           │
└─────────────────────────────────────────────────────────────────────┘
```

