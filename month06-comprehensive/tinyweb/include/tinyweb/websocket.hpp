// ============================================================================
// tinyweb/websocket.hpp — WebSocket 协议 (RFC 6455)
// ============================================================================
// WebSocket 帧格式 (RFC 6455 §5.2):
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-------+-+-------------+-------------------------------+
//  |F|R|R|R| opcode|M| Payload len  |    Extended payload length   |
//  |I|S|S|S|  (4)  |A|     (7)      |           16/64              |
//  |N|V|V|V|       |S|              |  (if payload len == 126/127) |
//  | |1|2|3|       |K|              |                              |
//  +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
//  |     Masking-key (if MASK set, 4 bytes)                        |
//  +---------------------------------------------------------------+
//  |     Payload Data (masked if MASK set)                         |
//  +---------------------------------------------------------------+
#pragma once
#include "common.hpp"    // std::string, std::optional, std::function
#include "buffer.hpp"     // Buffer (for WsConnection output)

// ── WebSocket 操作码 (RFC 6455 §5.2) ────────────────────────────────
enum class WsOpcode : uint8_t {
  CONTINUATION = 0x0,  // 分片续帧
  TEXT = 0x1,          // 文本帧 (UTF-8)
  BINARY = 0x2,        // 二进制帧
  CLOSE = 0x8,         // 关闭连接
  PING = 0x9,          // 心跳请求
  PONG = 0xA           // 心跳响应
};

// ── WebSocket 帧 ────────────────────────────────────────────────────
struct WebSocketFrame {
  bool fin = true;               // FIN: 是否是最后一帧 (分片时用)
  uint8_t rsv = 0;               // RSV1-3: 保留位 (扩展用, 必须为 0)
  WsOpcode opcode = WsOpcode::TEXT; // 操作码
  bool mask = false;             // MASK: payload 是否被掩码 (客户端→服务端必须 mask)
  uint8_t mask_key[4] = {};      // 掩码密钥 (4 字节)
  std::string payload;           // 负载数据

  // parse: 从字节流解析一个 WebSocket 帧
  // consumed: [out] 消耗的字节数 (调用者据此 drain buffer)
  // 返回 std::nullopt = 数据不够, 等下次
  static std::optional<WebSocketFrame> parse(const char* data, size_t len, size_t& consumed) {
    if (len < 2) return std::nullopt;             // 至少需要 2 字节 (FIN+opcode + mask+len)
    WebSocketFrame f;                             // 构建帧对象
    f.fin = (data[0] & 0x80) != 0;                // 字节0 bit7 = FIN
    f.rsv = (data[0] & 0x70) >> 4;                // 字节0 bit4-6 = RSV
    f.opcode = static_cast<WsOpcode>(data[0] & 0x0F); // 字节0 bit0-3 = opcode
    f.mask = (data[1] & 0x80) != 0;               // 字节1 bit7 = MASK
    uint64_t plen = data[1] & 0x7F;               // 字节1 bit0-6 = payload 长度 (7 bits)
    size_t header_len = 2;                        // 已读头部长度

    if (plen == 126) {                            // 126 = 后续 2 字节是实际长度 (16-bit)
      if (len < 4) return std::nullopt;           // 不够 → 等更多数据
      plen = ((uint8_t)data[2] << 8) | (uint8_t)data[3]; // 大端读 16-bit
      header_len = 4;
    } else if (plen == 127) {                     // 127 = 后续 8 字节是实际长度 (64-bit)
      if (len < 10) return std::nullopt;          // 不够 → 等更多数据
      plen = 0;
      for (int i = 0; i < 8; i++)                 // 大端读 64-bit
        plen = (plen << 8) | (uint8_t)data[2 + i];
      header_len = 10;
    }

    if (f.mask) {                                 // 如果有掩码
      if (len < header_len + 4) return std::nullopt; // 不够 → 等更多数据
      std::memcpy(f.mask_key, data + header_len, 4); // 复制 4 字节掩码密钥
      header_len += 4;                            // 头部 +4 字节
    }

    if (len < header_len + plen) return std::nullopt; // payload 数据不够 → 等更多
    f.payload.assign(data + header_len, plen);    // 提取 payload

    if (f.mask)                                   // 如果有掩码 → 逐字节 XOR 解码
      for (size_t i = 0; i < plen; i++)
        f.payload[i] ^= f.mask_key[i % 4];        // mask_key 循环使用

    consumed = header_len + plen;                 // 返回消耗的总字节数
    return f;                                     // 返回解析好的帧
  }

  // encode: 将帧编码为字节流 (用于发送)
  std::string encode() const {
    std::string s; s.reserve(10 + payload.size());// 预分配
    s.push_back((fin ? 0x80 : 0x00)               // 字节0: FIN + RSV + opcode
              | (rsv << 4) | (uint8_t(opcode) & 0x0F));
    uint8_t mb = mask ? 0x80 : 0x00;              // 字节1 bit7 = MASK (服务端→客户端不 mask)
    if (payload.size() < 126) {                   // 7-bit 长度够用
      s.push_back(mb | payload.size());
    } else if (payload.size() <= 0xFFFF) {        // 需要 16-bit 扩展长度
      s.push_back(mb | 126);                      // 126 标记
      s.push_back(payload.size() >> 8);           // 高字节
      s.push_back(payload.size() & 0xFF);         // 低字节
    } else {                                      // 需要 64-bit 扩展长度
      s.push_back(mb | 127);                      // 127 标记
      for (int i = 7; i >= 0; i--)                // 大端写 64-bit
        s.push_back((payload.size() >> (i * 8)) & 0xFF);
    }
    s += payload;                                 // 追加 payload
    return s;
  }
};

// ── WebSocket 连接状态 ───────────────────────────────────────────────
struct WebSocketState {
  using MessageCb = std::function<void(std::string_view, bool)>; // 消息回调 (数据, is_binary)
  using CloseCb = std::function<void()>;          // 关闭回调

  MessageCb on_message;                           // 收到消息时调用
  CloseCb on_close;                               // 收到 CLOSE 帧时调用
  std::string _fragment_buf;                      // 分片重组缓冲区 (CONTINUATION 帧拼接)

  // feed_frame: 处理一个接收到的帧
  void feed_frame(const WebSocketFrame& f) {
    switch (f.opcode) {                           // 根据操作码分发
    case WsOpcode::TEXT:                          // 文本帧
    case WsOpcode::BINARY:                        // 二进制帧
      if (!f.fin) { _fragment_buf += f.payload; return; } // 未完成 → 累积到缓冲区
      if (!_fragment_buf.empty()) {               // 有累积的分片 → 拼接后回调
        _fragment_buf += f.payload;               // 追加最后一帧
        on_message(_fragment_buf, f.opcode == WsOpcode::BINARY); // 通知
        _fragment_buf.clear();                    // 清空重组缓冲
      } else on_message(f.payload, f.opcode == WsOpcode::BINARY); // 单帧完整 → 直接通知
      break;

    case WsOpcode::CONTINUATION:                  // 分片续帧
      _fragment_buf += f.payload;                 // 追加到重组缓冲
      if (f.fin && on_message) {                  // 最后一帧 → 完成
        on_message(_fragment_buf, false);         // 假设 TEXT (实际应从首帧获取)
        _fragment_buf.clear();
      }
      break;

    case WsOpcode::PING:                          // 心跳请求 (自动回复 PONG 在 Connection 层处理)
      break;
    case WsOpcode::PONG:                          // 心跳响应 (忽略)
      break;
    case WsOpcode::CLOSE:                         // 关闭帧
      if (on_close) on_close();                   // 通知关闭
      break;
    default: break;                               // 其他 opcode 忽略
    }
  }
};

// ── WsConnection: WebSocket 连接对象 ──────────────────────────────────
struct WsConnection {
  int _fd;                                        // socket 文件描述符
  WebSocketState _ws;                             // WebSocket 协议状态
  Buffer _out_buf;                                // 输出缓冲区 (待发送帧)
  std::function<void()> _on_destroy;              // 销毁时回调 (用于 Connection 清理)

  explicit WsConnection(int fd) : _fd(fd) {}      // 构造函数: 绑定 fd

  // send_text: 发送文本消息
  void send_text(std::string_view msg) {
    WebSocketFrame f;                             // 构建 TEXT 帧
    f.opcode = WsOpcode::TEXT;                    // 文本操作码
    f.payload = std::string(msg);                 // 设置 payload
    auto encoded = f.encode();                    // 编码为字节流
    _out_buf.append(encoded.data(), encoded.size()); // 追加到输出缓冲
  }

  // send_pong: 发送 PONG 响应
  void send_pong() {
    WebSocketFrame f;                             // 构建 PONG 帧
    f.opcode = WsOpcode::PONG;                    // PONG 操作码
    f.payload = "pong";                           // 简单 payload
    auto encoded = f.encode();                    // 编码
    _out_buf.append(encoded.data(), encoded.size());
  }

  // close: 发送 CLOSE 帧并触发清理
  void close() {
    WebSocketFrame f;
    f.opcode = WsOpcode::CLOSE;
    auto encoded = f.encode();
    _out_buf.append(encoded.data(), encoded.size());
    if (_on_destroy) _on_destroy();               // 触发销毁回调 → Connection::close_conn()
  }
};
