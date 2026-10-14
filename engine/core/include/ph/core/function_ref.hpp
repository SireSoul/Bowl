#pragma once

#include <memory>
#include <type_traits>
#include <utility>

namespace ph {

// Non-owning reference to a callable: two pointers, no allocation. The callable must outlive the FunctionRef.
template <class Signature> class FunctionRef;

template <class R, class... Args> class FunctionRef<R(Args...)> {
public:
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, FunctionRef> && std::is_invocable_r_v<R, F&, Args...>)
    FunctionRef(F&& f) noexcept // NOLINT(google-explicit-constructor)
        : object_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
          call_([](void* o, Args... args) -> R {
              return (*static_cast<std::remove_reference_t<F>*>(o))(std::forward<Args>(args)...);
          }) {}

    R operator()(Args... args) const { return call_(object_, std::forward<Args>(args)...); }

private:
    void* object_;
    R (*call_)(void*, Args...);
};

} // namespace ph
