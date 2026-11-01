# Releasing the SDK

A tag is a version: `vX.Y.Z` a release, `vX.Y.0-beta.N` a pre-release leading to `vX.Y.0`.
Pushing a tag builds the lua mod for every platform and makes the GitHub release; a beta's is marked *Pre-release*, so the releases page's "Latest" stays the last release.
`tools/check-version.sh <tag>` checks that the tree says the version the tag names (CI runs it before releasing, and a mismatch releases nothing).

What says the version:

| | beta `v1.5.0-beta.2` | release `v1.5.0` |
|---|---|---|
| `SMS_MOD_API_MINOR` (`include/sms_mod.h`) | 5 | 5 |
| `SMS_MOD_API_PRERELEASE` | 2 | 0 |
| `version` in `lua/mod.ini` | `1.5.0-beta.2` | `1.5.0` |
| `project(... VERSION)` in `CMakeLists.txt` | `1.5.0` | `1.5.0` |
| `CHANGELOG.md` | a `### 1.5.0-beta.2` section under `## 1.5.0 (in beta)` | `## 1.5.0`, its betas' lists merged |

## A beta

Betas carry the work towards the next minor version, one after another, as often as there is something to try.

1. On `main`, the changes since the last beta are in `CHANGELOG.md` under a new `### 1.5.0-beta.N` heading (newest first, under `## 1.5.0 (in beta)`). A change to something an earlier beta added says so ("`set_outfit` takes -1 for as it is; it took 0 or 1 in beta 1").
2. Set `SMS_MOD_API_PRERELEASE` to N and `lua/mod.ini`'s version to `1.5.0-beta.N`. The first beta after a release also raises `SMS_MOD_API_MINOR`, the CMake version and the `## X.Y.0 (in beta)` heading.
3. `tools/check-version.sh v1.5.0-beta.N`, commit ("Release 1.5.0-beta.N") and get it onto `main` (pushed, or its PR merged), then tag that commit: `git tag v1.5.0-beta.N && git push origin v1.5.0-beta.N`.
4. In the port: move `sdk/` to the tag, and if the lua mod changed, pin its zip in `tools/mods/lua-mod.json` (the release page gives the MD5 and size). The game's beta builds then have it; its log says `[mods] API 1.5 beta N`.
5. Tell the beta's testers (the release page, the changelog section, and MODDING.md's "Pre-releases" for building against it).

Between betas, keep to what a release promises where you can (add at the ends; leave what is there alone): mods built against an earlier beta then keep working. Reshape what only betas have when it is worth it; those mods get the game's warning and are rebuilt.

## The release

1. Merge the betas' lists into one under `## 1.5.0` (drop "in beta" and the `###` headings; what a beta changed and a later one changed back need not be told).
2. `SMS_MOD_API_PRERELEASE` 0, `lua/mod.ini` version `1.5.0`, and the `GIT_TAG` in `README.md` and `docs/MODDING.md`'s quick start to `v1.5.0`; update "The current version" in MODDING.md's Compatibility.
3. `tools/check-version.sh v1.5.0`, commit ("Release 1.5.0") onto `main`, then tag it `v1.5.0` and push the tag.
4. In the port: `sdk/` and `lua-mod.json` to the release. In sms-mod-templates: the `GIT_TAG`s, then its own tag.

From then on 1.5 is fixed: the next change starts `1.6.0-beta.1`. A fix to the docs, the tools or the lua mod alone is a patch release (`v1.5.1`) with no beta.
