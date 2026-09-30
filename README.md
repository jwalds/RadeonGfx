# RadeonGfx — Polaris development fork

Fork of [X547/RadeonGfx](https://github.com/X547/RadeonGfx), a userland GPU
server that lets Mesa's RADV Vulkan driver run on Haiku. The upstream code
supports Southern Islands (GFX6) GPUs.

The `polaris` branch adds support for Polaris (GFX8, e.g. Radeon RX 460/560),
working together with the patched `radeon_hd` display driver from
[jwalds/haiku-radeon-polaris](https://github.com/jwalds/haiku-radeon-polaris).
The plan and test log live in that repository
(`docs/phase2-3d-plan.md`).

**Status:** early development, nothing works on Polaris yet.

## License

The upstream repository has no license file, so the original code remains
under its author's copyright. This fork is a personal development copy; a
license has been requested upstream. Do not redistribute modified versions
until that is resolved.

## AI note

The changes on the `polaris` branch are developed with AI assistance.
