#!/bin/bash
# DAgent 的 bwrap 包装，由 srt_bridge.mjs 作为 SRT 的 bwrapPath 使用。
# SRT 用可写 tmpfs 遮住 denyRead 目录：越界写入落进内存里的空目录并返回成功，命令结束即消失。
# 这里在全部挂载之后，把最上层仍是 tmpfs 的挂载点重挂为只读，让这类写入直接得到 EROFS。
# /tmp 是每次执行私有的临时目录，保持可写；叠在 tmpfs 之上的 bind 是独立挂载，不受影响。
real=$DAGENT_SRT_BWRAP
unset DAGENT_SRT_BWRAP

declare -A top=()
order=()

mounted() {
    [[ -n ${top["$2"]+set} ]] || order+=("$2")
    top["$2"]=$1
}

# 按 bwrap 选项的参数个数前进，并记录每个挂载点最上层的挂载类型。
step() {
    case $1 in
        --tmpfs) mounted tmpfs "$2"; width=2 ;;
        --bind|--ro-bind|--dev-bind|--bind-try|--ro-bind-try|--dev-bind-try|--bind-data|--ro-bind-data)
            mounted bind "$3"; width=3 ;;
        --dev|--proc|--mqueue) mounted bind "$2"; width=2 ;;
        --setenv|--symlink|--file|--chmod) width=3 ;;
        --args|--dir|--remount-ro|--chdir|--unsetenv|--cap-add|--cap-drop|--perms|--size|--uid|--gid|\
        --hostname|--argv0|--userns|--userns2|--pidns|--lock-file|--sync-fd|--seccomp|--add-seccomp-fd|\
        --block-fd|--userns-block-fd|--info-fd|--json-status-fd|--exec-label|--file-label) width=2 ;;
        *) width=1 ;;
    esac
}

args=("$@")
i=0
while (( i < ${#args[@]} )) && [[ ${args[i]} == --* && ${args[i]} != -- ]]; do
    step "${args[@]:i:3}"
    advance=$width
    # 挂载过多时 SRT 把挂载词放进 NUL 分隔的参数文件，bwrap 在 --args 的位置原样展开。
    if [[ ${args[i]} == --args ]]; then
        mapfile -d '' words < "/proc/self/fd/${args[i+1]}"
        for (( j = 0; j < ${#words[@]}; j += width )); do step "${words[@]:j:3}"; done
    fi
    (( i += advance ))
done

remount=()
for dest in "${order[@]}"; do
    [[ ${top["$dest"]} == tmpfs && $dest != /tmp ]] && remount+=(--remount-ro "$dest")
done
exec "$real" "${args[@]:0:i}" "${remount[@]}" "${args[@]:i}"
