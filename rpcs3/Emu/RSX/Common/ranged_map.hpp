#pragma once

#include <util/types.hpp>
#include "Utilities/address_range.h"

#include "util/pair_list.h"

#include <new>

namespace rsx
{
	template <typename T, int BlockSize>
	class ranged_map
	{
	protected:
		struct block_metadata_t
		{
			u32 id = umax;             // ID of the matadata blob
			u32 head_block = umax;     // Earliest block that may have an object that intersects with the data at the block with ID 'id'
		};

	public:
		// The surfaces starting in each block, in a short list (util/pair_list.c), where each was an STL
		// hash map. A block holds a few: a look along them is quicker than a hash and its modulo.
		// The value is made in the entry's pointer room and moves bitwise with it, as a unique_ptr can
		static_assert(sizeof(T) == sizeof(void*) && alignof(T) <= alignof(void*));

		// An entry as the callers see it: its address and its value
		struct entry_ref
		{
			u32 first;
			T& second;
		};

		struct entry_arrow
		{
			entry_ref ref;
			entry_ref* operator -> () { return &ref; }
		};

		static T& value_of(pair_list_entry_t& e)
		{
			return *std::launder(reinterpret_cast<T*>(e.room.bytes));
		}

		using inner_type = pair_list_t;
		using outer_type = typename std::array<inner_type, 0x100000000ull / BlockSize>;
		using metadata_array = typename std::array<block_metadata_t, 0x100000000ull / BlockSize>;

	protected:
		outer_type m_data{};
		metadata_array m_metadata;

		static pair_list_entry_t* entries(inner_type& block)
		{
			return PAIR_LIST_ENTRIES(&block);
		}

		static inline u32 block_for(u32 address)
		{
			return address / BlockSize;
		}

		static inline u32 block_address(u32 block_id)
		{
			return block_id * BlockSize;
		}

		void broadcast_insert(const utils::address_range32& range)
		{
			const auto head_block = block_for(range.start);
			for (auto meta = &m_metadata[head_block]; meta <= &m_metadata[block_for(range.end)]; ++meta)
			{
				if (head_block < meta->head_block)
				{
					meta->head_block = head_block;
				}
			}
		}

	public:
		class iterator
		{
			using super = typename rsx::ranged_map<T, BlockSize>;
			friend super;

		protected:
			inner_type* m_current = nullptr;
			inner_type* m_end = nullptr;

			inner_type* m_data_ptr = nullptr;
			block_metadata_t* m_metadata_ptr = nullptr;
			u32 m_index = 0;

			void forward_scan()
			{
				while (m_current < m_end)
				{
					if ((++m_current)->count) [[ likely ]]
					{
						m_index = 0;
						return;
					}
				}

				// end pointer
				m_current = nullptr;
				m_index = 0;
			}

			void next()
			{
				if (!m_current)
				{
					return;
				}

				if (++m_index < m_current->count) [[ likely ]]
				{
					return;
				}

				forward_scan();
			}

			void begin_range(u32 address, u32 index)
			{
				m_current = &m_data_ptr[address / BlockSize];
				m_end = m_current;
				m_index = index;
			}

			void begin_range(const utils::address_range32& range)
			{
				const auto start_block_id = range.start / BlockSize;
				const auto& metadata = m_metadata_ptr[start_block_id];
				m_current = &m_data_ptr[metadata.head_block < start_block_id ? metadata.head_block : start_block_id];
				m_end = &m_data_ptr[range.end / BlockSize];

				--m_current;
				forward_scan();
			}

			// The last moves into the gap, so the same index is looked at next
			void erase()
			{
				value_of(entries(*m_current)[m_index]).~T();
				pair_list_remove_at(m_current, m_index);

				if (m_index < m_current->count)
				{
					return;
				}

				forward_scan();
			}

			iterator(super* parent):
				m_data_ptr(parent->m_data.data()),
				m_metadata_ptr(parent->m_metadata.data())
			{}

		public:
			bool operator == (const iterator& other) const
			{
				return m_current == other.m_current && m_index == other.m_index;
			}

			entry_arrow operator -> () const
			{
				ensure(m_current);
				auto& e = entries(*m_current)[m_index];
				return { { e.key, value_of(e) } };
			}

			entry_ref operator * () const
			{
				ensure(m_current);
				auto& e = entries(*m_current)[m_index];
				return { e.key, value_of(e) };
			}

			iterator& operator ++ ()
			{
				ensure(m_current);
				next();
				return *this;
			}
		};

	public:
		ranged_map()
		{
			for (u32 i = 0; i < m_metadata.size(); i++)
			{
				m_metadata[i].id = i;
			}
		}

		ranged_map(const ranged_map&) = delete;
		ranged_map& operator=(const ranged_map&) = delete;

		~ranged_map()
		{
			clear();
		}

		void emplace(const utils::address_range32& range, T&& value)
		{
			broadcast_insert(range);
			auto& block = m_data[block_for(range.start)];

			if (const auto found = pair_list_find(&block, range.start))
			{
				value_of(*found) = std::move(value);
				return;
			}

			const auto added = ensure(pair_list_add(&block, range.start));
			new (added->room.bytes) T(std::move(value));
		}

		usz count(const u32 key) const
		{
			auto& block = const_cast<inner_type&>(m_data[block_for(key)]);
			return pair_list_find(&block, key) ? 1 : 0;
		}

		iterator find(const u32 key)
		{
			auto& block = m_data[block_for(key)];
			iterator ret = { this };

			if (const auto found = pair_list_find(&block, key))
			{
				ret.begin_range(key, static_cast<u32>(found - PAIR_LIST_ENTRIES(&block)));
			}

			return ret;
		}

		iterator erase(iterator& where)
		{
			where.erase();
			return where;
		}

		void erase(u32 address)
		{
			auto& block = m_data[block_for(address)];

			if (const auto found = pair_list_find(&block, address))
			{
				const u32 index = static_cast<u32>(found - PAIR_LIST_ENTRIES(&block));
				value_of(entries(block)[index]).~T();
				pair_list_remove_at(&block, index);
			}
		}

		iterator begin_range(const utils::address_range32& range)
		{
			iterator ret = { this };
			ret.begin_range(range);
			return ret;
		}

		iterator end()
		{
			iterator ret = { this };
			return ret;
		}

		void clear()
		{
			for (auto& block : m_data)
			{
				for (u32 i = 0; i < block.count; i++)
				{
					value_of(entries(block)[i]).~T();
				}
				pair_list_free(&block);
			}
		}
	};
}
