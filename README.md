# What's this?
A fork of the popular GLQuake descendant [QuakeSpasm](https://sourceforge.net/projects/quakespasm/) with a focus on high performance instead of broad compatibility, capable of handling even the most demanding [maps](https://www.quaddicted.com/reviews/ter_shibboleth_drake_redux.html) at very high framerates by utilizing more modern techniques such as:
- compute-based vis/frustum/backface culling
- compute-based lightmap updates
- better batching through multi-draw indirect rendering, instancing, and bindless textures
- clustered dynamic lighting
- persistent buffer mapping with manual synchronization

To avoid physics issue with high framerates the renderer is decoupled from the server (using code from QSS, via vkQuake). There are also a few other nice-to-have features such as the classic underwater warp effect, slightly higher color/depth buffer precision, reduced heap usage (can play shib1_drake out of the box), slightly faster savefile loading for large maps in complex [mods](https://www.moddb.com/mods/arcane-dimensions), capped framerate when no map is loaded, and a built-in ~hack~work-around for the z-fighting issues present in the original levels.

# Minimum requirements
- AMD Radeon HD 7000 series (GCN1, 2012) or newer
- Nvidia GeForce 600 series (Kepler, 2012) or newer
- Intel HD Graphics 500 (Skylake, 2015) or newer
