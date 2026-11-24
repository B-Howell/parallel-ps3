#include <util/types.hpp>

#include <utility>

namespace vk
{
	// Something to be deleted once the GPU is done with it: a pointer and the
	// function that deletes it (a plain function - nothing to allocate or copy)
	class disposable_t
	{
		using deleter_t = void (*)(void*);

		void* ptr;
		deleter_t deleter;

		disposable_t(void* ptr_, deleter_t deleter_) :
			ptr(ptr_), deleter(deleter_) {}
	public:

		disposable_t() = delete;
		disposable_t(const disposable_t&) = delete;

		disposable_t(disposable_t&& other) noexcept :
			ptr(std::exchange(other.ptr, nullptr)),
			deleter(other.deleter)
		{}

		~disposable_t()
		{
			if (ptr)
			{
				deleter(ptr);
				ptr = nullptr;
			}
		}

		// Hands the object over: this no longer deletes it, the taker does
		// by calling the function on the pointer
		void* take(deleter_t& deleter_out)
		{
			deleter_out = deleter;
			return std::exchange(ptr, nullptr);
		}

		// A call to make once the GPU is done: f(ptr), nothing allocated
		static disposable_t call(void* ptr_, deleter_t f)
		{
			return disposable_t(ptr_, f);
		}

		template <typename T>
		static disposable_t make(T* raw)
		{
			return disposable_t(raw, [](void* raw)
			{
				delete static_cast<T*>(raw);
			});
		}
	};

	struct garbage_collector
	{
		virtual void dispose(vk::disposable_t& object) = 0;

		// Called as the device goes away: a plain function, as each one is (it was a std::function)
		virtual void add_exit_callback(void (*callback)()) = 0;

		template<typename T>
		void dispose(std::unique_ptr<T>& object)
		{
			auto ptr = vk::disposable_t::make(object.release());
			dispose(ptr);
		}
	};

	garbage_collector* get_gc();
}
