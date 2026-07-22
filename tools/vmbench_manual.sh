#!/usr/bin/env bash
set -Eeuo pipefail

readonly REMOTE="${REMOTE:-origin}"
readonly PAGES="${PAGES:-4096}"
readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
readonly RESULT="${RESULT:-$REPO_ROOT/../xv6-vmbench-results/manual-$(date +%Y%m%d-%H%M%S)}"
readonly WORKTREE_ROOT="$(mktemp -d /tmp/xv6-vmbench-manual-XXXXXX)"

readonly -a VARIANTS=(pristine lazy cow main)
readonly -a BRANCHES=(
  concept/21-vmbench-pristine
  concept/21-vmbench-lazy
  concept/21-vmbench-cow
  concept/21-vmbench-main
)

created_worktrees=()

cleanup() {
  local worktree
  for worktree in "${created_worktrees[@]:-}"; do
    git -C "$REPO_ROOT" worktree remove --force "$worktree" >/dev/null 2>&1 || true
  done
  git -C "$REPO_ROOT" worktree prune >/dev/null 2>&1 || true
  rm -rf "$WORKTREE_ROOT"
}
trap cleanup EXIT INT TERM

require_command() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "缺少命令：$1" >&2
    exit 1
  }
}

for command in git make python3 qemu-system-riscv64 script; do
  require_command "$command"
done

mkdir -p "$RESULT/build" "$RESULT/logs" "$RESULT/charts"
: > "$RESULT/commits.txt"

echo "拉取实验分支..."
git -C "$REPO_ROOT" fetch --prune "$REMOTE"

run_variant() {
  local variant="$1"
  local branch="$2"
  local worktree="$WORKTREE_ROOT/$variant"
  local build_log="$RESULT/build/$variant.log"
  local qemu_log="$RESULT/logs/$variant.log"
  local count answer

  echo
  echo "============================================================"
  echo "实验基线：$variant"
  echo "远端分支：$REMOTE/$branch"
  echo "============================================================"

  git -C "$REPO_ROOT" worktree add --detach "$worktree" "$REMOTE/$branch"
  created_worktrees+=("$worktree")

  printf '%s %s %s\n' \
    "$variant" \
    "$branch" \
    "$(git -C "$worktree" rev-parse HEAD)" >> "$RESULT/commits.txt"

  echo "编译 $variant：make clean && make kernel/kernel fs.img"
  {
    make -C "$worktree" clean
    make -C "$worktree" kernel/kernel fs.img
  } >"$build_log" 2>&1

  while true; do
    rm -f "$qemu_log"

    echo
    echo "即将进入 $variant 的 xv6 shell。"
    echo "看到 \$ 提示符后，只需输入："
    echo
    echo "  vmbench all $PAGES"
    echo
    echo "等待输出结束并再次出现 \$ 后退出 QEMU："
    echo "  先按 Ctrl-A，松开，再按 X"
    echo
    read -r -p "按 Enter 启动 QEMU... " _

    (
      cd "$worktree"
      script -q -f -c "make qemu CPUS=1" "$qemu_log"
    )

    count="$(grep -a -c 'VMRESULT ' "$qemu_log" || true)"
    if [[ "$count" == "9" ]] \
      && ! grep -a -qE 'VMFAILED |VMERROR ' "$qemu_log"; then
      echo "[$variant] 采集成功：9 条 VMRESULT"
      break
    fi

    echo
    echo "[$variant] 采集不完整：发现 $count 条 VMRESULT，预期为 9 条。" >&2
    echo "日志：$qemu_log" >&2
    read -r -p "重新运行当前基线？[Y/n] " answer
    case "${answer:-Y}" in
      n|N|no|NO)
        exit 1
        ;;
    esac
  done
}

for index in "${!VARIANTS[@]}"; do
  run_variant "${VARIANTS[$index]}" "${BRANCHES[$index]}"
done

readonly MAIN_WORKTREE="$WORKTREE_ROOT/main"
readonly COMPARE_SCRIPT="$MAIN_WORKTREE/tools/vmbench_compare.py"

echo
echo "生成 CSV、Markdown 和 SVG..."
python3 "$COMPARE_SCRIPT" \
  "pristine=$RESULT/logs/pristine.log" \
  "lazy=$RESULT/logs/lazy.log" \
  "cow=$RESULT/logs/cow.log" \
  "main=$RESULT/logs/main.log" \
  --csv "$RESULT/vmbench-results.csv" \
  --markdown "$RESULT/vmbench-results.md" \
  --svg-dir "$RESULT/charts" \
  | tee "$RESULT/compare.stdout.md"

python3 - "$RESULT" "$PAGES" <<'PY'
from __future__ import annotations

import json
import sys
from datetime import datetime, timezone
from pathlib import Path

result = Path(sys.argv[1])
pages = int(sys.argv[2])
commits = []
for line in (result / "commits.txt").read_text(encoding="utf-8").splitlines():
    variant, branch, commit = line.split()
    commits.append({"variant": variant, "branch": branch, "commit": commit})
manifest = {
    "generated_at": datetime.now(timezone.utc).isoformat(),
    "pages": pages,
    "cpus": 1,
    "mode": "interactive-qemu",
    "records": commits,
}
(result / "manifest.json").write_text(
    json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
    encoding="utf-8",
)
PY

echo
echo "VMRESULT 数量："
grep -aHc 'VMRESULT ' "$RESULT"/logs/*.log

echo
echo "完成。结果目录："
echo "$RESULT"
echo
echo "主要产物："
printf '  %s\n' \
  "$RESULT/vmbench-results.csv" \
  "$RESULT/vmbench-results.md" \
  "$RESULT/compare.stdout.md" \
  "$RESULT/manifest.json" \
  "$RESULT/charts/"
echo
echo "打包命令："
echo "tar -C \"$(dirname "$RESULT")\" -czf \"$RESULT.tar.gz\" \"$(basename "$RESULT")\""
