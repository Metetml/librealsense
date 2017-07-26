#include "metadata-parser.h"
#include "archive.h"

namespace librealsense
{
    std::shared_ptr<sensor_interface> frame::get_sensor() const
    {
        auto res = sensor.lock();
        if (!res) return get_owner()->get_sensor();
        return res;
    }
    void frame::set_sensor(std::shared_ptr<sensor_interface> s) { sensor = s;}

    float3* points::get_vertices()
    {
        auto xyz = (float3*)data.data();
        return xyz;
    }

    size_t points::get_vertex_count() const
    {
        return data.size() / (sizeof(float3) + sizeof(int2));
    }

    int2* points::get_pixel_coordinates()
    {
        auto xyz = (float3*)data.data();
        auto ijs = (int2*)(xyz + get_vertex_count());
        return ijs;
    }

    // Defines general frames storage model
    template<class T>
    class frame_archive : public std::enable_shared_from_this<frame_archive<T>>, public archive_interface
    {
        std::atomic<uint32_t>* max_frame_queue_size;
        std::atomic<uint32_t> published_frames_count;
        small_heap<T, RS2_USER_QUEUE_SIZE> published_frames;

        callbacks_heap callback_inflight;

        std::vector<T> freelist; // return frames here
        std::atomic<bool> recycle_frames;
        int pending_frames = 0;
        std::recursive_mutex mutex;
        std::shared_ptr<platform::time_service> _time_service;
        std::shared_ptr<metadata_parser_map> _metadata_parsers = nullptr;

        std::weak_ptr<sensor_interface> _sensor;
        std::shared_ptr<sensor_interface> get_sensor() const override { return _sensor.lock(); }
        void set_sensor(std::shared_ptr<sensor_interface> s) override { _sensor = s; }

        T alloc_frame(const size_t size, const frame_additional_data& additional_data, bool requires_memory)
        {
            T backbuffer;
            //const size_t size = modes[stream].get_image_size(stream);
            {
                std::lock_guard<std::recursive_mutex> guard(mutex);

                if (requires_memory)
                {
                    // Attempt to obtain a buffer of the appropriate size from the freelist
                    for (auto it = begin(freelist); it != end(freelist); ++it)
                    {
                        if (it->data.size() == size)
                        {
                            backbuffer = std::move(*it);
                            freelist.erase(it);
                            break;
                        }
                    }
                }

                // Discard buffers that have been in the freelist for longer than 1s
                for (auto it = begin(freelist); it != end(freelist);)
                {
                    if (additional_data.timestamp > it->additional_data.timestamp + 1000) it = freelist.erase(it);
                    else ++it;
                }
            }

            if (requires_memory)
            {
                backbuffer.data.resize(size, 0); // TODO: Allow users to provide a custom allocator for frame buffers
            }
            backbuffer.additional_data = additional_data;
            return backbuffer;
        }

        frame_interface* track_frame(frame& f)
        {
            std::unique_lock<std::recursive_mutex> lock(mutex);

            auto published_frame = f.publish(this->shared_from_this());
            if (published_frame)
            {
                published_frame->acquire();
                return published_frame;
            }

            LOG_DEBUG("publish(...) failed");
            return nullptr;
        }

        void unpublish_frame(frame_interface* frame)
        {
            if (frame)
            {
                auto f = (T*)frame;
                log_frame_callback_end(f);
                std::unique_lock<std::recursive_mutex> lock(mutex);

                --published_frames_count;

                if (recycle_frames)
                {
                    freelist.push_back(std::move(*f));
                }
                lock.unlock();

                published_frames.deallocate(f);
            }
        }

        frame_interface* publish_frame(frame_interface* frame)
        {
            auto f = (T*)frame;

            if (published_frames_count >= *max_frame_queue_size)
            {
                LOG_DEBUG("User didn't release frame resource.");
                return nullptr;
            }
            auto new_frame = published_frames.allocate();
            if (new_frame)
            {
                ++published_frames_count;
                *new_frame = std::move(*f);
            }

            return new_frame;
        }

        void log_frame_callback_end(T* frame) const
        {
            if (frame)
            {
                auto callback_ended = _time_service?_time_service->get_time():0;
                auto callback_warning_duration = 1000 / (frame->additional_data.fps + 1);
                auto callback_duration = callback_ended - frame->get_frame_callback_start_time_point();

                LOG_DEBUG("CallbackFinished," << librealsense::get_string(frame->get_stream_type()) << "," << frame->get_frame_number()
                    << ",DispatchedAt," << callback_ended);

                if (callback_duration > callback_warning_duration)
                {
                    LOG_DEBUG("Frame Callback [" << librealsense::get_string(frame->get_stream_type())
                             << "#" << std::dec << frame->additional_data.frame_number
                             << "] overdue. (Duration: " << callback_duration
                             << "ms, FPS: " << frame->additional_data.fps << ", Max Duration: " << callback_warning_duration << "ms)");
                }
            }
        }

        std::shared_ptr<metadata_parser_map> get_md_parsers() const { return _metadata_parsers; };

        friend class frame;

    public:
        explicit frame_archive(std::atomic<uint32_t>* in_max_frame_queue_size,
                             std::shared_ptr<platform::time_service> ts,
                             std::shared_ptr<metadata_parser_map> parsers)
            : max_frame_queue_size(in_max_frame_queue_size),
              mutex(), recycle_frames(true), _time_service(ts),
              _metadata_parsers(parsers)
        {
            published_frames_count = 0;
        }

        callback_invocation_holder begin_callback()
        {
            return { callback_inflight.allocate(), &callback_inflight };
        }

        void release_frame_ref(frame_interface* ref)
        {
            ref->release();
        }

        frame_interface* alloc_and_track(const size_t size, const frame_additional_data& additional_data, bool requires_memory)
        {
            auto frame = alloc_frame(size, additional_data, requires_memory);
            return track_frame(frame);
        }

        void flush()
        {
            published_frames.stop_allocation();
            callback_inflight.stop_allocation();
            recycle_frames = false;

            auto callbacks_inflight = callback_inflight.get_size();
            if (callbacks_inflight > 0)
            {
                LOG_WARNING(callbacks_inflight << " callbacks are still running on some other threads. Waiting until all callbacks return...");
            }
            // wait until user is done with all the stuff he chose to borrow
            callback_inflight.wait_until_empty();

            {
                std::lock_guard<std::recursive_mutex> guard(mutex);
                freelist.clear();
            }

            pending_frames = published_frames.get_size();
            if (pending_frames > 0)
            {
                LOG_WARNING("The user was holding on to "
                    << std::dec << pending_frames << " frames after stream 0x"
                    << std::hex << this << " stopped" << std::dec);
            }
            // frames and their frame refs are not flushed, by design
        }

        ~frame_archive()
        {
            if (pending_frames > 0)
            {
                LOG_WARNING("All frames from stream 0x"
                    << std::hex << this << " are now released by the user");
            }
        }

    };

    std::shared_ptr<archive_interface> make_archive(rs2_extension type,
                                                    std::atomic<uint32_t>* in_max_frame_queue_size,
                                                    std::shared_ptr<platform::time_service> ts,
                                                    std::shared_ptr<metadata_parser_map> parsers)
    {
        switch(type)
        {
        case RS2_EXTENSION_VIDEO_FRAME :
            return std::make_shared<frame_archive<video_frame>>(in_max_frame_queue_size, ts, parsers);

        case RS2_EXTENSION_COMPOSITE_FRAME :
            return std::make_shared<frame_archive<composite_frame>>(in_max_frame_queue_size, ts, parsers);

        default:
            throw std::runtime_error("Requested frame type is not supported!");
        }
    }
}

void frame::release()
{
    if (ref_count.fetch_sub(1) == 1)
    {
        on_release();
        owner->unpublish_frame(this);
    }
}

frame_interface* frame::publish(std::shared_ptr<archive_interface> new_owner)
{
    owner = new_owner;
    return owner->publish_frame(this);
}

rs2_metadata_t frame::get_frame_metadata(const rs2_frame_metadata& frame_metadata) const
{
    auto md_parsers = owner->get_md_parsers();

    if (!md_parsers)
        throw invalid_value_exception(to_string() << "metadata not available for "
                                      << get_string(get_stream_type())<<" stream");

    auto it = md_parsers.get()->find(frame_metadata);
    if (it == md_parsers.get()->end())          // Possible user error - md attribute is not supported by this frame type
        throw invalid_value_exception(to_string() << get_string(frame_metadata)
                                      << " attribute is not applicable for "
                                      << get_string(get_stream_type()) << " stream ");

    // Proceed to parse and extract the required data attribute
    return it->second->get(*this);
}

bool frame::supports_frame_metadata(const rs2_frame_metadata& frame_metadata) const
{
    auto md_parsers = owner->get_md_parsers();

    // verify preconditions
    if (!md_parsers)
        return false;                         // No parsers are available or no metadata was attached

    auto it = md_parsers.get()->find(frame_metadata);
    if (it == md_parsers.get()->end())          // Possible user error - md attribute is not supported by this frame type
        return false;

    return it->second->supports(*this);
}

const byte* frame::get_frame_data() const
{
    const byte* frame_data = data.data();

    if (on_release.get_data())
    {
        frame_data = static_cast<const byte*>(on_release.get_data());
    }

    return frame_data;
}

rs2_timestamp_domain frame::get_frame_timestamp_domain() const
{
    return additional_data.timestamp_domain;
}

rs2_time_t frame::get_frame_timestamp() const
{
    return additional_data.timestamp;
}

unsigned long long frame::get_frame_number() const
{
    return additional_data.frame_number;
}

rs2_time_t frame::get_frame_system_time() const
{
    return additional_data.system_time;
}

int frame::get_framerate() const
{
    return additional_data.fps;
}

void frame::update_frame_callback_start_ts(rs2_time_t ts)
{
    additional_data.frame_callback_started = ts;
}

rs2_format frame::get_format() const
{
    return additional_data.format;
}
rs2_stream frame::get_stream_type() const
{
    return additional_data.stream_type;
}

rs2_time_t frame::get_frame_callback_start_time_point() const
{
    return additional_data.frame_callback_started;
}

void frame::log_callback_start(rs2_time_t timestamp)
{
    update_frame_callback_start_ts(timestamp);
    LOG_DEBUG("CallbackStarted," << std::dec<< librealsense::get_string(get_stream_type()) << "," << get_frame_number() << ",DispatchedAt," << timestamp);
}

void frame::log_callback_end(rs2_time_t timestamp) const
{
    auto callback_warning_duration = 1000.f / (get_framerate() + 1);
    auto callback_duration = timestamp - get_frame_callback_start_time_point();

    LOG_DEBUG("CallbackFinished," << librealsense::get_string(get_stream_type()) << "," << get_frame_number() << ",DispatchedAt," << timestamp);

    if (callback_duration > callback_warning_duration)
    {
        LOG_INFO("Frame Callback " << librealsense::get_string(get_stream_type())
                 << "#" << std::dec << get_frame_number()
                 << "overdue. (Duration: " << callback_duration
                 << "ms, FPS: " << get_framerate() << ", Max Duration: " << callback_warning_duration << "ms)");
    }
}
