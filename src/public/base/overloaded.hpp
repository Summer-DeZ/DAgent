#pragma once

namespace dagent::base {

/// 将多个可调用对象的 operator() 合并为一个重载集合。
template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

} // namespace dagent::base
