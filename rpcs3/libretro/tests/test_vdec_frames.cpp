// cellVdec's decoded frames (Emu/Cell/Modules/cellVdec.cpp): plain data,
// the AU's in a retro_deque made with the decoder and the game's in a ring
// the decoder thread puts into and the game takes from with no lock, where
// there were std::deques of frames that each owned their picture through a
// shared_ptr to the pool, behind the context's mutex.
// A picture's buffer now goes back to the pool by hand, so:
//
// - a frame queued and taken as cellVdecGetPictureExt takes it comes out
//   in order, with the picture it went in with;
// - every way a frame leaves a queue gives its buffer back: taken by the
//   game, flushed by a new sequence or a close, left by an aborted AU, or
//   still queued when the decoder is destroyed - no buffer is lost, and
//   none is given back twice;
// - the queues are the size they were made, after thousands of pictures;
// - with the decoder thread and the game on two threads, a new sequence
//   flushing now and then, the game gets the pictures in order, none
//   twice, the decoder never has more than out_max queued, and every
//   buffer ends up back in the pool.
//
// cellVdec.cpp is compiled into this test (the decoder's context is
// private to it); the rest is the core's own objects and archives, the
// same link as the core's with this main. Built by `make check`.

#include "../../Emu/Cell/Modules/cellVdec.cpp"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <thread>
#include <atomic>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// Buffers resting in the pool
static usz pooled(const vdec_pic_pool& pool)
{
	usz count = 0;
	for (const auto& slot : pool.slots)
		count += slot.load() != nullptr;
	return count;
}

static vdec_frame frame_of(vdec_context& vdec, u64 n)
{
	vdec_frame frame;
	frame.seq_id = 1;
	frame.cmd_id = n;
	frame.pts = n * 3003;
	frame.pic = vdec.pic_pool.take();
	return frame;
}

int main()
{
	{
		vdec_context vdec(CELL_VDEC_CODEC_TYPE_AVC, 0, 0, 0, vm::null, 0);
		CHECK(pooled(vdec.pic_pool) == 0);

		// Decoded, queued for the game, taken by it: in order, and the
		// buffers end up in the pool - as many as were ever in flight
		std::set<vdc_pic*> seen;
		u64 next_in = 0, next_out = 0, wrong = 0;
		for (u32 step = 0; step < 5000; step++)
		{
			// An AU's pictures
			const u32 pictures = 1 + step % 3;
			for (u32 i = 0; i < pictures; i++)
			{
				const vdec_frame frame = frame_of(vdec, next_in++);
				seen.insert(frame.pic);
				CHECK(vdec_frames_push_back_growing(&vdec.decoded, &frame));
			}
			// On to the output queue while there is room
			while (!vdec_frames_empty(&vdec.decoded) && vdec.out_count() <= vdec.out_max)
			{
				vdec.out_push(*vdec_frames_front(&vdec.decoded));
				vdec_frames_pop_front(&vdec.decoded);
			}
			// The game takes some
			CHECK(vdec.out_begin());
			vdec_frame frame;
			for (u32 i = 0; i < 1 + step % 4 && vdec.out_take(frame); i++)
			{
				wrong += frame.cmd_id != next_out || frame.pts != next_out * 3003 || !frame.pic;
				next_out++;
				vdec.give_back(frame);
				wrong += frame.pic != nullptr;
			}
			vdec.out_end();
		}
		CHECK(wrong == 0 && next_out > 4000);
		CHECK(seen.size() <= vdec_pic_pool::size);

		const usz in_flight = vdec.out_count() + vdec_frames_count(&vdec.decoded);
		CHECK(pooled(vdec.pic_pool) + in_flight == seen.size());

		// A new sequence, or a close, flushes the output queue; an aborted
		// AU leaves pictures behind: all their buffers go back
		for (u64 n = 0; n < 10; n++)
		{
			const vdec_frame frame = frame_of(vdec, n);
			seen.insert(frame.pic);
			if (n < 7)
				vdec.out_push(frame);
			else
				CHECK(vdec_frames_push_back_growing(&vdec.decoded, &frame));
		}
		CHECK(vdec.out_count() >= 7 && vdec_frames_count(&vdec.decoded) >= 3);
		CHECK(pooled(vdec.pic_pool) + 10 <= seen.size());
		// The decoder thread's flush; the game skips past it when it looks
		vdec.out_flush();
		vdec.flush(vdec.decoded);
		CHECK(vdec.out_begin());
		vdec_frame none;
		CHECK(!vdec.out_take(none));
		vdec.out_end();
		CHECK(vdec.out_count() == 0 && vdec_frames_empty(&vdec.decoded));
		CHECK(pooled(vdec.pic_pool) == seen.size());

		// Taken again, they are the same buffers: none was freed or lost
		std::vector<vdec_frame> again;
		for (usz i = 0; i < seen.size(); i++)
		{
			again.push_back(frame_of(vdec, i));
			CHECK(seen.count(again.back().pic) == 1);
		}
		CHECK(pooled(vdec.pic_pool) == 0);

		// Given back once: a second give of the same frame does nothing
		for (vdec_frame& frame : again)
		{
			vdec.give_back(frame);
			vdec.give_back(frame);
		}
		CHECK(pooled(vdec.pic_pool) == seen.size());

		// Left queued when the decoder goes: the destructor returns them
		// (a leak or a double free here is the sanitizer's to find)
		for (u64 n = 0; n < 10; n++)
		{
			const vdec_frame frame = frame_of(vdec, n);
			if (n & 1)
				vdec.out_push(frame);
			else
				CHECK(vdec_frames_push_back_growing(&vdec.decoded, &frame));
		}
	}

	// The decoder thread and the game on two threads
	{
		vdec_context vdec(CELL_VDEC_CODEC_TYPE_AVC, 0, 0, 0, vm::null, 0);
		constexpr u64 total = 200000;
		std::atomic<u64> epoch{0}; // pictures before this cmd_id were flushed
		std::atomic<bool> done{false};
		std::atomic<u32> over{0};
		std::set<vdc_pic*> seen;

		std::thread decoder([&]
		{
			for (u64 n = 0; n < total; n++)
			{
				// Room, as the decoder thread waits for it
				while (vdec.out_count() > vdec.out_max)
				{
					const retro_waitaddr_t room[1] = {{&vdec.out_taken, static_cast<u32>(retro_atomic_load_acquire_int(&vdec.out_taken))}};
					if (vdec.out_count() > vdec.out_max)
						retro_waitaddr_wait(room, 1, 1000);
				}
				if (n % 5000 == 4999)
				{
					// A new sequence: what is queued is not the game's any more
					epoch = n;
					vdec.out_flush();
				}
				vdec_frame frame;
				frame.seq_id = 1;
				frame.cmd_id = n;
				frame.pic = vdec.pic_pool.take();
				seen.insert(frame.pic);
				vdec.out_push(frame);
				if (vdec.out_count() > vdec.out_max + 1)
					over++;
			}
			done = true;
		});

		u64 taken = 0, wrong = 0, last = 0;
		bool any = false;
		while (!done || vdec.out_count())
		{
			if (!vdec.out_begin())
				continue;
			vdec_frame frame;
			while (vdec.out_take(frame))
			{
				// In order; a flushed one never comes after the flush that
				// dropped it was seen
				if ((any && frame.cmd_id <= last) || !frame.pic)
					wrong++;
				any = true;
				last = frame.cmd_id;
				taken++;
				vdec.give_back(frame);
			}
			vdec.out_end();
		}
		decoder.join();

		CHECK(wrong == 0);
		CHECK(over == 0);
		CHECK(taken > total / 2);
		CHECK(seen.size() <= vdec_pic_pool::size);
		CHECK(pooled(vdec.pic_pool) == seen.size());
		std::printf("test_vdec_frames: decoder thread and game apart: %llu of %llu pictures taken (the rest flushed), %llu out of order, %u over out_max, %zu buffers, all back in the pool: %s\n",
			static_cast<unsigned long long>(taken), static_cast<unsigned long long>(total), static_cast<unsigned long long>(wrong), over.load(), seen.size(), pooled(vdec.pic_pool) == seen.size() ? "yes" : "no");
	}

	std::printf("test_vdec_frames: %s\n", failures ? "FAIL" : "ok");
	std::fflush(stdout);
	std::_Exit(failures ? 1 : 0);
}
