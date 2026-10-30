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
CODEBUDDY_EXT=/root/.local/share/CodeBuddyExtension
VSCODE_SERVER_MACHINE=/root/.vscode-server/data/Machine

# CodeBuddy Code 各版本目录结构不一致，按"排除表"筛选而不是写死清单，
# 只排除可重建或体积大的运行时数据。
CODEBUDDY_SKIP=" logs traces debug diagnostics blobs file-history sessions shell-snapshots plugins "

# 目录被搬进漫游目录后，内部相对软链会因层级变化失效，这里按原路径改写为绝对路径（幂等）。
# 参数：<漫游目录> <该目录在容器内的原路径>
# realpath 用 -s：只做字符串规范化，不解析路径中间的软链，否则基准会被二次解析。
absolutize_links() {
    [ -d "$1" ] || return 0
    find "$1" -type l 2>/dev/null | while IFS= read -r _link; do
        _target=$(readlink "$_link")
        case "$_target" in
            /*) continue ;;
        esac
        _rel=${_link#"$1"/}
        ln -sfn "$(realpath -ms "$(dirname "$2/$_rel")/$_target")" "$_link"
    done
    return 0
}

# link_dir <目标目录> <漫游目录内相对路径>
link_dir() {
    target=$1
    real="$ROAM/$2"
    mkdir -p "$real" "$(dirname "$target")"
    if [ -L "$target" ]; then
        absolutize_links "$real" "$target"
        [ "$(readlink "$target")" = "$real" ] && return 0
        rm -f "$target"
    elif [ -d "$target" ]; then
        # 首次运行：把镜像或旧环境里已有的内容并入漫游目录，不覆盖已有文件。
        cp -an "$target/." "$real/" 2>/dev/null || true
        absolutize_links "$real" "$target"
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

    # CodeBuddy 技能实体（~/.agents 里的 skills，~/.codebuddy/skills 以软链引用）。
    link_dir /root/.agents "agents"

    # CodeBuddy 插件：只保留 Data（设置、登录态、对话历史）；Cache/Logs 可重建。
    link_dir "$CODEBUDDY_EXT/Data" "CodeBuddyExtension/Data"

    # CodeBuddy Code CLI：常见配置文件即使当前还没生成也先建链，保证之后写入直接落进漫游目录。
    # 文件生成前这些链是悬空的（等同文件不存在），属预期状态。
    for name in settings.json settings.local.json CODEBUDDY.md mcp.json history.jsonl expert-history.json; do
        link_file "$CODEBUDDY_CLI/$name" "codebuddy/$name"
    done

    # 再按实际存在的条目补齐：不同版本的目录结构不一致，命中排除表的留在容器内。
    if [ -d "$CODEBUDDY_CLI" ]; then
        for _elf2_entry in "$CODEBUDDY_CLI"/*; do
            if [ ! -e "$_elf2_entry" ] && [ ! -L "$_elf2_entry" ]; then continue; fi
            _elf2_name=${_elf2_entry##*/}
            case " $CODEBUDDY_SKIP " in
                *" $_elf2_name "*) continue ;;
            esac
            if [ -d "$_elf2_entry" ]; then
                link_dir "$_elf2_entry" "codebuddy/$_elf2_name"
            else
                link_file "$_elf2_entry" "codebuddy/$_elf2_name"
            fi
        done
        unset _elf2_entry _elf2_name
    fi

    # VSCode / Cursor Remote-SSH 的机器级设置。
    link_file "$VSCODE_SERVER_MACHINE/settings.json" "vscode-server/Machine/settings.json"
fi

echo
echo "漫游目录：$ROAM"
if [ -d "$ROAM" ]; then
    du -sh "$ROAM" || true
    echo "（CNB 文件漫游上限约 100 MB，超出会在环境销毁时报错）"
fi
