# HTTP 下载工具
#
# 为 curl/wget 提供统一下载宏，并带重试逻辑。
# 任何需要 HTTP 下载的 mk 文件都应先 include 本文件。

ifndef _MK_HTTP_INCLUDED
_MK_HTTP_INCLUDED := 1

# HTTP 下载工具探测：优先使用 curl，找不到时回退到 wget。
CURL := $(shell command -v curl 2>/dev/null)
WGET := $(shell command -v wget 2>/dev/null)

# 探测 wget 是否支持 --show-progress（GNU wget 扩展）。
WGET_HAS_PROGRESS := $(if $(WGET),$(shell $(WGET) --help 2>&1 | grep -q show-progress && echo 1))

# 选择 HTTP 工具并定义统一命令。
# GH_TOKEN 环境变量可启用 GitHub API 认证请求，避免匿名请求 60 次/小时限制。
ifdef CURL
    HTTP_TOOL := curl
    # 获取 URL 内容并输出到 stdout，常用于解析 API 响应。
    # 如果存在 GH_TOKEN，则使用认证请求。
    # $(1)：URL
    HTTP_GET = curl -fsSL $(if $(GH_TOKEN),-H "Authorization: Bearer $(GH_TOKEN)") $(1) 2>/dev/null
    # 带进度条和重试的文件下载。
    # $(1)：URL，$(2)：输出文件
    HTTP_DOWNLOAD = curl -fSL --retry 3 --retry-delay 2 --progress-bar $(1) -o $(2)
    # 静默下载文件，并带重试。
    # $(1)：URL，$(2)：输出文件
    HTTP_DOWNLOAD_QUIET = curl -fsSL --retry 3 --retry-delay 2 $(1) -o $(2)
else ifdef WGET
    HTTP_TOOL := wget
    HTTP_GET = wget -q $(if $(GH_TOKEN),--header="Authorization: Bearer $(GH_TOKEN)") -O- $(1) 2>/dev/null
    HTTP_DOWNLOAD = wget -q $(if $(WGET_HAS_PROGRESS),--show-progress) --tries=3 --waitretry=2 $(1) -O $(2)
    HTTP_DOWNLOAD_QUIET = wget -q --tries=3 --waitretry=2 $(1) -O $(2)
else
    HTTP_TOOL :=
endif

endif # _MK_HTTP_INCLUDED
