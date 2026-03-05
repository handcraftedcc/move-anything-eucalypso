# Eucalypso Release Workflow (Tag-Based, CI Build)

This project releases from Git tags. CI builds and publishes the artifact.
Do not rely on local build output for official releases.

## Preconditions

- Working tree is clean.
- Release changes are merged into `main`.
- `.github/workflows/release.yml` is configured for `push.tags: v*`.

## Standard Release Flow

1. Update module version in `src/module.json` (for example `0.1.6`).
2. Commit version/docs/release changes to `main`.
3. Push `main`.
4. Create an annotated release tag:
   - `git tag -a v0.1.6 -m "Eucalypso v0.1.6"`
5. Push the tag:
   - `git push origin v0.1.6`
6. Wait for GitHub Actions `Release` workflow to finish for `v0.1.6`.
7. Verify GitHub release exists and has `eucalypso-module.tar.gz`.

## Verification Commands

- Confirm HEAD and tag:
  - `git log --oneline --decorate -n 3`
  - `git show --oneline v0.1.6`
- Confirm workflow run:
  - `gh run list --repo handcraftedcc/move-anything-eucalypso --workflow Release --limit 5`
- Confirm published release:
  - `gh release view v0.1.6 --repo handcraftedcc/move-anything-eucalypso`

## If Tag Points To Wrong Commit

1. Delete local tag:
   - `git tag -d v0.1.6`
2. Delete remote tag:
   - `git push origin :refs/tags/v0.1.6`
3. Recreate tag at correct commit and push again.

## If Release Workflow Does Not Trigger

1. Re-push the tag:
   - `git push origin :refs/tags/v0.1.6`
   - `git push origin v0.1.6`
2. If still blocked, run workflow manually on tag ref:
   - `gh workflow run release.yml --repo handcraftedcc/move-anything-eucalypso --ref v0.1.6`
3. Re-check workflow and release status.

## Notes

- Official artifact should come from GitHub Actions release job.
- Manual `gh release create ...` should be fallback-only.
