#!/bin/sh
# 把 WebIDE / CodeBuddy / Remote-SSH 的个人状态软链到 ~/.cnb，交给 CNB 的文件漫游
# 在环境销毁时保存、重建时恢复。
#
# 由 .cnb.yml 的云原生开发 stages 在启动时调用，可重复执行。
#   sh 10-env/persist.sh            建立软链
#   sh 10-env/persist.sh --report   只报告当前漫游目录占用
#
# 注意：漫游只在环境正常销毁时发生（`kill 1` 会丢失），且总量有上限（约 100 MB）。
set -eu

REPO=${CNB_REPO_SLUG:-VibraReveal}
ROAM="/root/.cnb/$REPO"
CODE_SERVER_USER=/root/.local/share/code-server/User
CODEBUDDY_CLI=/root/.codebuddy
VSCODE_SERVER_MACHINE=/root/.vscode-server/data/Machine

# link_dir <目标目录> <漫游目录内相对路径>
link_dir() {
    target=$1
    real="$ROAM/$2"
    mkdir -p "$real" "$(dirname "$target")"
    if [ -L "$target" ]; then
        [ "$(readlink "$target")" = "$real" ] && return 0
        rm -f "$target"
    elif [ -d "$target" ]; then
        # 首次运行：把镜像或旧环境里已有的内容并入漫游目录，不覆盖已有文件。
        cp -an "$target/." "$real/" 2>/dev/null || true
        rm -rf "$target"
    fi
    ln -s "$real" "$target"
    echo "已软链目录 $target -> $real"
}

# link_file <目标文件> <漫游目录内相对路径>
link_file() {
    target=$1
    real="$ROAM/$2"
    mkdir -p "$(dirname "$real")" "$(dirname "$target")"
    if [ -L "$target" ]; then
        [ "$(readlink "$target")" = "$real" ] && return 0
        rm -f "$target"
    elif [ -f "$target" ]; then
        [ -f "$real" ] || cp -a "$target" "$real"
        rm -f "$target"
    fi
    ln -s "$real" "$target"
    echo "已软链文件 $target -> $real"
}

if [ "${1:-}" != "--report" ]; then
    # code-server 的用户级 settings.json / keybindings.json / snippets 由 CNB 原生漫游，
    # 这里只处理不漫游的部分。globalStorage 只挑 CodeBuddy 一个扩展目录，
    # 避免把语言服务、模型缓存等大目录一起送进漫游配额。
    link_dir "$CODE_SERVER_USER/globalStorage/tencent-cloud.coding-copilot" \
             "code-server/globalStorage/tencent-cloud.coding-copilot"
    link_dir "$CODE_SERVER_USER/History" "code-server/History"
    link_file "$CODE_SERVER_USER/Machine/settings.json" "code-server/Machine/settings.json"

    # CodeBuddy 插件：设置、登录态与对话历史。
    link_dir /root/.local/share/CodeBuddyExtension "CodeBuddyExtension"

    # CodeBuddy Code CLI：只漫游配置与历史，日志、追踪、快照等可重建内容留在容器内。
    for name in settings.json settings.local.json CODEBUDDY.md mcp.json history.jsonl; do
        link_file "$CODEBUDDY_CLI/$name" "codebuddy/$name"
    done
    for name in projects rules skills agents commands; do
        link_dir "$CODEBUDDY_CLI/$name" "codebuddy/$name"
    done

    # VSCode / Cursor Remote-SSH 的机器级设置。
    link_file "$VSCODE_SERVER_MACHINE/settings.json" "vscode-server/Machine/settings.json"
fi

echo
echo "漫游目录：$ROAM"
if [ -d "$ROAM" ]; then
    du -sh "$ROAM" || true
    echo "（CNB 文件漫游上限约 100 MB，超出会在环境销毁时报错）"
fi
