# Architecture diagrams

| Topic | README image | Editable source |
|---|---|---|
| Manifest, action scheduler, tools and cache | [Pipeline](images/system-architecture.svg) | [draw.io](images/system-architecture.drawio) |
| Included source/header/object/library example | [Dependency graph](images/dependency-graph.svg) | [draw.io](images/dependency-graph.drawio) |
| Reuse, verified restore or real rebuild | [Cache decisions](images/cache-decisions.svg) | [draw.io](images/cache-decisions.drawio) |

Native white rectangles, black text and simple arrows follow the style of the other project READMEs. Open the draw.io XML files in diagrams.net to edit individual nodes/labels/edges, then export the matching SVG. No AI-generated raster images are used.

The dependency example includes header-file edges for explanation; `cppbuild graph` exports runnable action nodes only. Header hashes are discovered inside each compile action. The cache diagram describes per-action publication, not a transaction over the entire build. See ARCHITECTURE.md for exact invariants and scope.
