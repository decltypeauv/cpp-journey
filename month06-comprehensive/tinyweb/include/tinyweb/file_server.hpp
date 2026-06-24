// ============================================================================
// tinyweb/file_server.hpp — 静态文件服务
// ============================================================================
// 特性: MIME 类型自动检测, 目录→index.html, 路径遍历防护
#pragma once
#include "common.hpp"             // std::string, std::unordered_map, fs (filesystem)
#include "http_response.hpp"      // HttpResponse (Builder 模式)

class FileServer {
  std::string _root;                               // 静态文件根目录
  std::unordered_map<std::string, std::string> _mime_types; // 扩展名 → MIME 类型 映射

public:
  // 构造函数: 设置根目录并初始化 MIME 表
  explicit FileServer(std::string root) : _root(std::move(root)) {
    _mime_types = {                                // 常见 MIME 类型表
      {".html", "text/html"}, {".css", "text/css"},
      {".js", "application/javascript"}, {".json", "application/json"},
      {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
      {".gif", "image/gif"}, {".svg", "image/svg+xml"},
      {".ico", "image/x-icon"}, {".txt", "text/plain"},
      {".xml", "application/xml"}, {".pdf", "application/pdf"},
      {".woff2", "font/woff2"}, {".wasm", "application/wasm"},
    };
  }

  // serve: 根据 URL 路径返回对应的静态文件
  HttpResponse serve(const std::string& path) const {
    // ── 安全检查: 防止 ../ 路径遍历攻击 ──────────────────────────
    std::string safe_path = sanitize(path);        // 过滤危险字符
    if (safe_path.empty())                         // 包含非法字符?
      return HttpResponse{}.set_status(403)        // → 403 Forbidden
             .set_json(R"({"error":"Forbidden"})");

    // ── 构建完整文件路径 ────────────────────────────────────────
    fs::path file_path = fs::path(_root) / fs::path(safe_path);

    // ── 目录 → 自动查找 index.html ──────────────────────────────
    if (fs::is_directory(file_path))               // 请求的是目录?
      file_path /= "index.html";                   // → 追加 index.html

    // ── 文件存在性检查 ──────────────────────────────────────────
    if (!fs::exists(file_path) ||                  // 不存在?
        !fs::is_regular_file(file_path))           // 不是普通文件 (可能为 symlink/device)?
      return HttpResponse{}.set_status(404)        // → 404 Not Found
             .set_json(R"({"error":"File Not Found"})");

    // ── 读取文件内容 ────────────────────────────────────────────
    std::ifstream f(file_path, std::ios::binary | std::ios::ate); // 二进制模式 + 定位到末尾
    if (!f)                                        // 打开失败?
      return HttpResponse{}.set_status(500)
             .set_json(R"({"error":"Internal Error"})");
    auto size = f.tellg();                         // 获取文件大小 (因为 ios::ate)
    f.seekg(0);                                    // 回到开头
    std::string content(size, '\0');               // 预分配 string
    f.read(content.data(), size);                  // 一次读入

    // ── 检测 MIME 类型 ──────────────────────────────────────────
    std::string ext = file_path.extension().string(); // 提取扩展名
    std::string mime = "application/octet-stream";   // 默认二进制类型
    auto it = _mime_types.find(ext);               // 查 MIME 表
    if (it != _mime_types.end()) mime = it->second;  // 找到 → 使用对应 MIME
    // 文本类型追加 charset
    if (ext == ".html" || ext == ".css" || ext == ".js" ||
        ext == ".json" || ext == ".xml" || ext == ".txt")
      mime += "; charset=utf-8";

    return HttpResponse{}.set_status(200)          // 返回 200 + 文件内容
           .set_content_type(mime)
           .set_body(std::move(content));
  }

private:
  // sanitize: 过滤路径中的危险字符, 防止路径遍历
  std::string sanitize(const std::string& path) const {
    std::string result;
    for (char c : path) {                          // 逐字符检查
      if (c == '/' || c == '.' || c == '-' || c == '_' || std::isalnum(c))
        result += c;                               // 安全字符 → 保留
      else
        return "";                                 // 非法字符 → 拒绝整个路径
    }
    if (result.find("..") != std::string::npos)    // 包含 .. (父目录)?
      return "";                                   // → 拒绝
    if (!result.empty() && result[0] == '/')       // 去掉开头的 /
      result.erase(0, 1);
    return result;
  }
};
