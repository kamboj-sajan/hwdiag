#include "common/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace hwdiag::json {

Value Value::object() {
  Value v;
  v.type_ = Type::Object;
  return v;
}

Value Value::array() {
  Value v;
  v.type_ = Type::Array;
  return v;
}

Value& Value::operator[](std::string_view key) {
  if (type_ == Type::Null) type_ = Type::Object;
  for (size_t i = 0; i < keys_.size(); ++i) {
    if (keys_[i] == key) return items_[i];
  }
  keys_.emplace_back(key);
  items_.emplace_back();
  return items_.back();
}

const Value* Value::find(std::string_view key) const {
  if (type_ != Type::Object) return nullptr;
  for (size_t i = 0; i < keys_.size(); ++i) {
    if (keys_[i] == key) return &items_[i];
  }
  return nullptr;
}

void Value::push_back(Value v) {
  if (type_ == Type::Null) type_ = Type::Array;
  items_.push_back(std::move(v));
}

size_t Value::size() const { return items_.size(); }

namespace {

// Length of the valid UTF-8 sequence starting at s[i], or 0 if invalid.
size_t utf8_sequence_length(std::string_view s, size_t i) {
  const auto c = static_cast<unsigned char>(s[i]);
  size_t len = 0;
  if (c >= 0xC2 && c <= 0xDF) {
    len = 2;
  } else if (c >= 0xE0 && c <= 0xEF) {
    len = 3;
  } else if (c >= 0xF0 && c <= 0xF4) {
    len = 4;
  } else {
    return 0;
  }
  if (i + len > s.size()) return 0;
  for (size_t k = 1; k < len; ++k) {
    if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 0;
  }
  return len;
}

void newline(std::string& out, int indent, int depth) {
  if (indent < 0) return;
  out.push_back('\n');
  out.append(static_cast<size_t>(indent * depth), ' ');
}

}  // namespace

std::string quote(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else if (c < 0x80) {
          out.push_back(static_cast<char>(c));
        } else {
          const size_t len = utf8_sequence_length(s, i);
          if (len == 0) {
            out += "\\ufffd";
            ++i;
            continue;
          }
          out.append(s.substr(i, len));
          i += len;
          continue;
        }
    }
    ++i;
  }
  out.push_back('"');
  return out;
}

std::string format_number(double d) {
  if (!std::isfinite(d)) return "null";
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof buf, d);
  return std::string(buf, res.ptr);
}

void Value::write(std::string& out, int indent, int depth) const {
  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Int: out += std::to_string(i_); break;
    case Type::UInt: out += std::to_string(u_); break;
    case Type::Double: out += format_number(d_); break;
    case Type::String: out += quote(s_); break;
    case Type::Array:
      if (items_.empty()) {
        out += "[]";
        break;
      }
      out.push_back('[');
      for (size_t i = 0; i < items_.size(); ++i) {
        if (i) out.push_back(',');
        newline(out, indent, depth + 1);
        items_[i].write(out, indent, depth + 1);
      }
      newline(out, indent, depth);
      out.push_back(']');
      break;
    case Type::Object:
      if (items_.empty()) {
        out += "{}";
        break;
      }
      out.push_back('{');
      for (size_t i = 0; i < items_.size(); ++i) {
        if (i) out.push_back(',');
        newline(out, indent, depth + 1);
        out += quote(keys_[i]);
        out += indent < 0 ? ":" : ": ";
        items_[i].write(out, indent, depth + 1);
      }
      newline(out, indent, depth);
      out.push_back('}');
      break;
  }
}

std::string Value::dump(int indent) const {
  std::string out;
  write(out, indent, 0);
  return out;
}

}  // namespace hwdiag::json
