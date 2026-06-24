// ============================================================================
// tinyweb/buffer.hpp — I/O 读写缓冲 (libevent evbuffer 风格)
// ============================================================================
// 设计灵感: libevent 的 evbuffer (链式缓冲区)
// 简化版: 单块连续内存 + read_pos/write_pos 指针
// 关键优化: 大数据自动扩容, 读取后 compact 避免无限增长
#pragma once
#include "common.hpp"    // 系统头文件

class Buffer {
  std::vector<char> _buf;       // 底层存储 (单块连续内存)
  size_t _read_pos = 0;         // 读指针: 下一个待读取字节的位置
  size_t _write_pos = 0;        // 写指针: 下一个可写入字节的位置

public:
  // 构造函数: 预分配 4KB (典型页面大小)
  Buffer() { _buf.resize(4096); }

  // append: 追加数据到写缓冲区 (用于构建 HTTP 响应)
  void append(const char* data, size_t len) {
    if (_write_pos + len > _buf.size())           // 容量不够?
      _buf.resize(_write_pos + len + 4096);       // 扩容: 需要量 + 4KB margin
    std::memcpy(_buf.data() + _write_pos, data, len); // 拷贝到写位置
    _write_pos += len;                            // 推进写指针
  }
  void append(std::string_view s) { append(s.data(), s.size()); } // string_view 重载

  // read_from: 从 socket fd 读取数据到写缓冲区
  // 返回: >0=读取字节数, 0=对端关闭, <0=错误 (EAGAIN=暂无数据)
  ssize_t read_from(int fd) {
    if (_write_pos + 4096 > _buf.size())          // 预留至少 4KB 空间
      _buf.resize(_buf.size() * 2);              // 不够则翻倍扩容
    ssize_t n = recv(fd, _buf.data() + _write_pos, // 从 fd 读入
                     _buf.size() - _write_pos, 0);
    if (n > 0) _write_pos += n;                  // 成功: 推进写指针
    return n;
  }

  // write_to: 把读缓冲区中的数据写出到 socket fd
  // 返回: >0=写入字节数, 0=暂无数据, <0=错误
  ssize_t write_to(int fd) {
    size_t avail = _write_pos - _read_pos;        // 可写数据量
    if (avail == 0) return 0;                     // 没有待写数据
    ssize_t n = send(fd, _buf.data() + _read_pos, avail, MSG_NOSIGNAL); // 写到 fd (避免 SIGPIPE)
    if (n > 0) {
      _read_pos += n;                             // 推进读指针
      if (_read_pos == _write_pos)                // 所有数据都已写出
        _read_pos = _write_pos = 0;              // 重置双指针 (复用缓冲区)
      else if (_read_pos > 4096) {                // 读指针偏移过大
        std::memmove(_buf.data(), _buf.data() + _read_pos, // 将未写出的数据移到开头
                     _write_pos - _read_pos);
        _write_pos -= _read_pos;                  // 调整写指针
        _read_pos = 0;                            // 重置读指针
      }
    }
    return n;
  }

  // 观察器
  std::string_view view() const {                 // 查看当前可读数据 (不消费)
    return {_buf.data() + _read_pos, _write_pos - _read_pos};
  }
  size_t size() const { return _write_pos - _read_pos; } // 可读数据量
  void clear() { _read_pos = _write_pos = 0; }   // 清空 (重置双指针, 不释放内存)
};
