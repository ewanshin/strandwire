#!/bin/bash
# Build the branch that goes to the public repository: the commits of `work` with their English
# commit messages. The private repository keeps Korean messages; the English text of each commit
# is a git note under refs/notes/en (git notes --ref=en add -m "..." <commit>). A commit without
# a note keeps its message. Run from the repository root in git bash; then
#   git push --force public public-en:master
#
# 공개 저장소에 올릴 브랜치를 만든다: `work`의 커밋에 영어 커밋 메시지를 붙인 것. 사설 저장소는
# 한국어 메시지를 유지하고, 각 커밋의 영어 메시지는 refs/notes/en 아래의 git note다
# (git notes --ref=en add -m "..." <commit>). 노트가 없는 커밋은 메시지를 그대로 둔다.
# 저장소 루트에서 git bash로 실행한 뒤
#   git push --force public public-en:master
set -e

missing=$(git rev-list work | while read c; do git notes --ref=en show "$c" > /dev/null 2>&1 || echo "$c"; done)
if [ -n "$missing" ]; then
    echo "commits without an English note (refs/notes/en):"
    echo "$missing" | while read c; do git log -1 --format='  %h %s' "$c"; done
    exit 1
fi

git branch -f public-en work
FILTER_BRANCH_SQUELCH_WARNING=1 git filter-branch -f \
    --msg-filter 'git notes --ref=en show "$GIT_COMMIT" 2>/dev/null || cat' public-en > /dev/null
echo "public-en is ready:"
git log --oneline -3 public-en
