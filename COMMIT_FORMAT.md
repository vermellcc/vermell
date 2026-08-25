# Commit & Branch Format

We use a relaxed and simple format for commits and branches to keep our workflow agile and readable.

## Commits

**Format:**
`<type>: [<scope>] <subject>`

**Examples:**
- `fix: router matching issue resolved`
- `feat: added new logging middleware`
- `docs: update readme instructions`

### Types
- **feat**: A new feature.
- **fix**: A bug fix.
- **docs**: Documentation changes.
- **style**: Formatting, missing semi colons, etc.
- **refactor**: Code changes that neither fix a bug nor add a feature.
- **perf**: Code changes that improve performance.
- **test**: Adding missing tests or correcting existing ones.
- **chore**: Minor maintenance or tooling updates.

## Branches

Branch names should clearly identify the type of work, the author, and a brief description.

**Format:**
`<type>/<username>-<description>`

**Examples:**
- `feat/jdoe-router-fix`
- `fix/asmith-auth-crash`
- `chore/mgarcia-update-deps`