/**
 * RTSP stream reading implementation for MP4 writer
 * Simplified version that follows rtsp_recorder example
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavutil/mathematics.h>

#include "core/logger.h"
#include "core/shutdown_coordinator.h"
#include "video/mp4_writer.h"
#include "video/mp4_writer_internal.h"
#include "database/database_manager.h"
#include "database/db_recordings.h"

// Thread-related fields for the MP4 writer
typedef struct {
    pthread_t thread;         // Recording thread
    int running;              // Flag indicating if the thread is running
    char rtsp_url[MAX_PATH_LENGTH]; // URL of the RTSP stream to record
    volatile sig_atomic_t shutdown_requested; // Flag indicating if shutdown was requested
    mp4_writer_t *writer;     // MP4 writer instance
    int segment_duration;     // Duration of each segment in seconds
    time_t last_segment_time; // Time when the last segment was created
} mp4_writer_thread_t;

// Structure to track segment information
typedef struct {
    int segment_index;
    bool has_audio;
    bool last_frame_was_key;  // Flag to indicate if the last frame of previous segment was a key frame
} segment_info_t;

/**
 * Record an RTSP stream to an MP4 file for a specified duration
 * 
 * This function handles the actual recording of an RTSP stream to an MP4 file.
 * It maintains a single RTSP connection across multiple recording segments,
 * ensuring there are no gaps between segments.
 * 
 * Error handling:
 * - Network errors: The function will return an error code, but the input context
 *   will be preserved if possible so that the caller can retry.
 * - File system errors: The function will attempt to clean up resources and return
 *   an error code.
 * - Timestamp errors: The function uses a robust timestamp handling approach to
 *   prevent floating point errors and timestamp inflation.
 * 
 * @param rtsp_url The URL of the RTSP stream to record
 * @param output_file The path to the output MP4 file
 * @param duration The duration to record in seconds
 * @param input_ctx_ptr Pointer to an existing input context (can be NULL)
 * @param has_audio Flag indicating whether to include audio in the recording
 * @param prev_segment_info Optional pointer to previous segment information for timestamp continuity
 * @return 0 on success, negative value on error
 */
int record_segment(const char *rtsp_url, const char *output_file, int duration, 
                  AVFormatContext **input_ctx_ptr, int has_audio, 
                  segment_info_t *prev_segment_info) {
    int ret = 0;
    AVFormatContext *input_ctx = NULL;
    AVFormatContext *output_ctx = NULL;
    AVDictionary *opts = NULL;
    AVDictionary *out_opts = NULL;
    AVPacket pkt;
    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    AVStream *out_video_stream = NULL;
    AVStream *out_audio_stream = NULL;
    int64_t first_video_dts = AV_NOPTS_VALUE;
    int64_t first_video_pts = AV_NOPTS_VALUE;
    int64_t first_audio_dts = AV_NOPTS_VALUE;
    int64_t first_audio_pts = AV_NOPTS_VALUE;
    int64_t last_video_dts = 0;
    int64_t last_video_pts = 0;
    int64_t last_audio_dts = 0;
    int64_t last_audio_pts = 0;
    int audio_packet_count = 0;
    int video_packet_count = 0;
    int64_t start_time;
    time_t last_progress = 0;
    int segment_index = 0;
    
    // Initialize segment index if previous segment info is provided
    if (prev_segment_info) {
        segment_index = prev_segment_info->segment_index + 1;
        log_info("Starting new segment with index %d", segment_index);
    }
    
    log_info("Recording from %s", rtsp_url);
    log_info("Output file: %s", output_file);
    log_info("Duration: %d seconds", duration);
    
    // Use existing input context if provided
    if (*input_ctx_ptr) {
        input_ctx = *input_ctx_ptr;
        log_debug("Using existing input context");
    } else {
        // Set up RTSP options for low latency
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);  // Use TCP for RTSP (more reliable than UDP)
        av_dict_set(&opts, "fflags", "nobuffer", 0);     // Reduce buffering
        av_dict_set(&opts, "flags", "low_delay", 0);     // Low delay mode
        av_dict_set(&opts, "max_delay", "500000", 0);    // Maximum delay of 500ms
        av_dict_set(&opts, "stimeout", "5000000", 0);    // Socket timeout in microseconds (5 seconds)
        
        // Open input
    ret = avformat_open_input(&input_ctx, rtsp_url, NULL, &opts);
    if (ret < 0) {
        char error_buf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, error_buf, AV_ERROR_MAX_STRING_SIZE);
        log_error("Failed to open input: %d (%s)", ret, error_buf);
        
        // Ensure input_ctx is NULL after a failed open
        input_ctx = NULL;
        
        // Don't quit, just return an error code so the caller can retry
        goto cleanup;
    }
        
        // Find stream info
        ret = avformat_find_stream_info(input_ctx, NULL);
        if (ret < 0) {
            log_error("Failed to find stream info: %d", ret);
            goto cleanup;
        }
        
        // Store the input context for reuse
        *input_ctx_ptr = input_ctx;
    }
    
    // Log input stream info
    log_debug("Input format: %s", input_ctx->iformat->name);
    log_debug("Number of streams: %d", input_ctx->nb_streams);
    
    // Find video and audio streams
    for (unsigned int i = 0; i < input_ctx->nb_streams; i++) {
        AVStream *stream = input_ctx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx < 0) {
            video_stream_idx = i;
            log_debug("Found video stream: %d", i);
            log_debug("  Codec: %s", avcodec_get_name(stream->codecpar->codec_id));
            log_debug("  Resolution: %dx%d", stream->codecpar->width, stream->codecpar->height);
            if (stream->avg_frame_rate.num && stream->avg_frame_rate.den) {
                log_debug("  Frame rate: %.2f fps", 
                       (float)stream->avg_frame_rate.num / stream->avg_frame_rate.den);
            }
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_idx < 0) {
            audio_stream_idx = i;
            log_debug("Found audio stream: %d", i);
            log_debug("  Codec: %s", avcodec_get_name(stream->codecpar->codec_id));
            log_debug("  Sample rate: %d Hz", stream->codecpar->sample_rate);
            // Handle channel count for different FFmpeg versions
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
            // For FFmpeg 5.0 and newer
            log_debug("  Channels: %d", stream->codecpar->ch_layout.nb_channels);
#else
            // For older FFmpeg versions
            log_debug("  Channels: %d", stream->codecpar->channels);
#endif
        }
    }
    
    if (video_stream_idx < 0) {
        log_error("No video stream found");
        ret = -1;
        goto cleanup;
    }
    
    // Create output context
    ret = avformat_alloc_output_context2(&output_ctx, NULL, "mp4", output_file);
    if (ret < 0 || !output_ctx) {
        log_error("Failed to create output context: %d", ret);
        goto cleanup;
    }
    
    // Add video stream
    out_video_stream = avformat_new_stream(output_ctx, NULL);
    if (!out_video_stream) {
        log_error("Failed to create output video stream");
        ret = -1;
        goto cleanup;
    }
    
    // Copy video codec parameters
    ret = avcodec_parameters_copy(out_video_stream->codecpar, 
                                 input_ctx->streams[video_stream_idx]->codecpar);
    if (ret < 0) {
        log_error("Failed to copy video codec parameters: %d", ret);
        goto cleanup;
    }
    
    // Set video stream time base
    out_video_stream->time_base = input_ctx->streams[video_stream_idx]->time_base;
    
    // Add audio stream if available and audio is enabled
    if (audio_stream_idx >= 0 && has_audio) {
        log_info("Including audio stream in MP4 recording");
        out_audio_stream = avformat_new_stream(output_ctx, NULL);
        if (!out_audio_stream) {
            log_error("Failed to create output audio stream");
            ret = -1;
            goto cleanup;
        }
        
        // Copy audio codec parameters
        ret = avcodec_parameters_copy(out_audio_stream->codecpar, 
                                     input_ctx->streams[audio_stream_idx]->codecpar);
        if (ret < 0) {
            log_error("Failed to copy audio codec parameters: %d", ret);
            goto cleanup;
        }
        
        // Set audio stream time base
        out_audio_stream->time_base = input_ctx->streams[audio_stream_idx]->time_base;
    }
    
    // CRITICAL FIX: Disable faststart to prevent segmentation faults
    // The faststart option causes a second pass that moves the moov atom to the beginning of the file
    // This second pass is causing segmentation faults during shutdown
    av_dict_set(&out_opts, "movflags", "empty_moov", 0);
    
    // Open output file
    ret = avio_open(&output_ctx->pb, output_file, AVIO_FLAG_WRITE);
    if (ret < 0) {
        log_error("Failed to open output file: %d", ret);
        goto cleanup;
    }
    
    // Write file header
    ret = avformat_write_header(output_ctx, &out_opts);
    if (ret < 0) {
        log_error("Failed to write header: %d", ret);
        goto cleanup;
    }
    
    // Initialize packet
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;
    
    // Start recording
    start_time = av_gettime();
    log_info("Recording started...");
    
    // Flag to track if we've found the first key frame
    bool found_first_keyframe = false;
    // Flag to track if we're waiting for the final key frame
    bool waiting_for_final_keyframe = false;
    // Flag to track if shutdown was detected
    bool shutdown_detected = false;
    
    // Main recording loop
    while (1) {
        // Check if shutdown has been initiated
        if (!shutdown_detected && !waiting_for_final_keyframe && is_shutdown_initiated()) {
            log_info("Shutdown initiated, waiting for next key frame to end recording");
            waiting_for_final_keyframe = true;
            shutdown_detected = true;
        }
        
        // Check if we've reached the duration limit
        if (duration > 0 && !waiting_for_final_keyframe && !shutdown_detected) {
            int64_t elapsed_seconds = (av_gettime() - start_time) / 1000000;
            
            // If we've reached the duration limit, wait for the next key frame
            if (elapsed_seconds >= duration) {
                log_info("Reached duration limit of %d seconds, waiting for next key frame to end recording", duration);
                waiting_for_final_keyframe = true;
            }
            // If we're close to the duration limit (within 1 second), also wait for the next key frame
            // This helps ensure we don't wait too long for a key frame at the end of a segment
            else if (elapsed_seconds >= duration - 1) {
                log_info("Within 1 second of duration limit (%d seconds), waiting for next key frame to end recording", duration);
                waiting_for_final_keyframe = true;
            }
        }
        
        // Read packet
        ret = av_read_frame(input_ctx, &pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                log_info("End of stream reached");
                break;
            } else if (ret != AVERROR(EAGAIN)) {
                log_error("Error reading frame: %d", ret);
                break;
            }
            // EAGAIN means try again, so we continue
            av_usleep(10000);  // Sleep 10ms to avoid busy waiting
            continue;
        }
        
        // Process video packets
        if (pkt.stream_index == video_stream_idx) {
            // Check if this is a key frame
            bool is_keyframe = (pkt.flags & AV_PKT_FLAG_KEY) != 0;
            
            // If we're waiting for the first key frame
            if (!found_first_keyframe) {
                // If the previous segment ended with a key frame, we can start immediately
                // Otherwise, wait for a key frame
                if (prev_segment_info && prev_segment_info->last_frame_was_key && segment_index > 0) {
                    log_info("Previous segment ended with a key frame, starting new segment immediately");
                    found_first_keyframe = true;
                    
                    // Reset start time to now
                    start_time = av_gettime();
                } else if (is_keyframe) {
                    log_info("Found first key frame, starting recording");
                    found_first_keyframe = true;
                    
                    // Reset start time to when we found the first key frame
                    start_time = av_gettime();
                } else {
                    // For regular segments, always wait for a key frame
                    // Skip this frame as we're waiting for a key frame
                    av_packet_unref(&pkt);
                    continue;
                }
            }
            
            // If we're waiting for the final key frame to end recording
            if (waiting_for_final_keyframe) {
                // Check if this is a key frame or if we've been waiting too long
                static int64_t waiting_start_time = 0;
                
                // Initialize waiting start time if not set
                if (waiting_start_time == 0) {
                    waiting_start_time = av_gettime();
                }
                
                // Calculate how long we've been waiting for a key frame
                int64_t wait_time = (av_gettime() - waiting_start_time) / 1000000;
                
                // If this is a key frame or we've waited too long (more than 2 seconds)
                if (is_keyframe || wait_time > 2) {
                    if (is_keyframe) {
                        log_info("Found final key frame, ending recording");
                        // Set flag to indicate the last frame was a key frame
                        if (prev_segment_info) {
                            prev_segment_info->last_frame_was_key = true;
                            log_debug("Last frame was a key frame, next segment will start immediately");
                        }
                    } else {
                        log_info("Waited %lld seconds for key frame, ending recording with non-key frame", (long long)wait_time);
                        // Clear flag since the last frame was not a key frame
                        if (prev_segment_info) {
                            prev_segment_info->last_frame_was_key = false;
                        }
                    }
                    
                    // Process this final frame and then break the loop
                    // Initialize first DTS if not set
                    if (first_video_dts == AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE) {
                        first_video_dts = pkt.dts;
                        first_video_pts = pkt.pts != AV_NOPTS_VALUE ? pkt.pts : pkt.dts;
                        log_debug("First video DTS: %lld, PTS: %lld", 
                                (long long)first_video_dts, (long long)first_video_pts);
                    }
                    
                    // Handle timestamps based on segment index
                    if (segment_index == 0) {
                        // First segment - adjust timestamps relative to first_dts
                        if (pkt.dts != AV_NOPTS_VALUE && first_video_dts != AV_NOPTS_VALUE) {
                            pkt.dts -= first_video_dts;
                            if (pkt.dts < 0) pkt.dts = 0;
                        }
                        
                        if (pkt.pts != AV_NOPTS_VALUE && first_video_pts != AV_NOPTS_VALUE) {
                            pkt.pts -= first_video_pts;
                            if (pkt.pts < 0) pkt.pts = 0;
                        }
                    } else {
                        // Subsequent segments - maintain timestamp continuity
                        // CRITICAL FIX: Use a small fixed offset instead of carrying over potentially large timestamps
                        // This prevents the timestamp inflation issue while still maintaining continuity
                        if (pkt.dts != AV_NOPTS_VALUE && first_video_dts != AV_NOPTS_VALUE) {
                            // Calculate relative timestamp within this segment
                            int64_t relative_dts = pkt.dts - first_video_dts;
                            // Add a small fixed offset (e.g., 1/30th of a second in timebase units)
                            // This ensures continuity without timestamp inflation
                            pkt.dts = relative_dts + 1;
                        }
                        
                        if (pkt.pts != AV_NOPTS_VALUE && first_video_pts != AV_NOPTS_VALUE) {
                            int64_t relative_pts = pkt.pts - first_video_pts;
                            pkt.pts = relative_pts + 1;
                        }
                    }
                    
    // CRITICAL FIX: Ensure PTS >= DTS for video packets to prevent "pts < dts" errors
    // This is essential for MP4 format compliance and prevents ghosting artifacts
    if (pkt.pts != AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE && pkt.pts < pkt.dts) {
        log_debug("Fixing video packet with PTS < DTS: PTS=%lld, DTS=%lld", 
                 (long long)pkt.pts, (long long)pkt.dts);
        pkt.pts = pkt.dts;
    }
    
            // CRITICAL FIX: Ensure DTS values don't exceed MP4 format limits (0x7fffffff)
            // This prevents the "Assertion next_dts <= 0x7fffffff failed" error
            if (pkt.dts != AV_NOPTS_VALUE) {
                if (pkt.dts > 0x7fffffff) {
                    log_warn("DTS value exceeds MP4 format limit: %lld, resetting to safe value", (long long)pkt.dts);
                    // Reset to a small value that maintains continuity
                    pkt.dts = 1000;
                    if (pkt.pts != AV_NOPTS_VALUE) {
                        // Maintain PTS-DTS relationship if possible
                        int64_t pts_dts_diff = pkt.pts - pkt.dts;
                        if (pts_dts_diff >= 0) {
                            pkt.pts = pkt.dts + pts_dts_diff;
                        } else {
                            pkt.pts = pkt.dts;
                        }
                    } else {
                        pkt.pts = pkt.dts;
                    }
                }
                
                // Additional check to ensure DTS is always within safe range
                // This handles cases where DTS might be close to the limit
                if (pkt.dts > 0x70000000) {  // ~75% of max value
                    log_info("DTS value approaching MP4 format limit: %lld, resetting to prevent overflow", (long long)pkt.dts);
                    // Reset to a small value
                    pkt.dts = 1000;
                    if (pkt.pts != AV_NOPTS_VALUE) {
                        // Maintain PTS-DTS relationship
                        pkt.pts = pkt.dts + 1;
                    } else {
                        pkt.pts = pkt.dts;
                    }
                }
            }
    
    // CRITICAL FIX: Ensure packet duration is within reasonable limits
    // This prevents the "Packet duration is out of range" error
    if (pkt.duration > 10000000) {
        log_warn("Packet duration too large: %lld, capping at reasonable value", (long long)pkt.duration);
        // Cap at a reasonable value (e.g., 1 second in timebase units)
        pkt.duration = 90000;
    }
                    
                    // Update last timestamps
                    if (pkt.dts != AV_NOPTS_VALUE) {
                        last_video_dts = pkt.dts;
                    }
                    if (pkt.pts != AV_NOPTS_VALUE) {
                        last_video_pts = pkt.pts;
                    }
                    
                    // Explicitly set duration for the final frame to prevent segmentation fault
                    if (pkt.duration == 0 || pkt.duration == AV_NOPTS_VALUE) {
                        // Use the time base of the video stream to calculate a reasonable duration
                        if (input_ctx->streams[video_stream_idx]->avg_frame_rate.num > 0 && 
                            input_ctx->streams[video_stream_idx]->avg_frame_rate.den > 0) {
                            // Calculate duration based on framerate (time_base units)
                            pkt.duration = av_rescale_q(1, 
                                                       av_inv_q(input_ctx->streams[video_stream_idx]->avg_frame_rate),
                                                       input_ctx->streams[video_stream_idx]->time_base);
                        } else {
                            // Default to a reasonable value if framerate is not available
                            pkt.duration = 1;
                        }
                        log_debug("Set final frame duration to %lld", (long long)pkt.duration);
                    }
                    
                    // Set output stream index
                    pkt.stream_index = out_video_stream->index;
                    
                    // Write packet
                    ret = av_interleaved_write_frame(output_ctx, &pkt);
                    if (ret < 0) {
                        log_error("Error writing video frame: %d", ret);
                    }
                    
                    // Break the loop after processing the final frame
                    av_packet_unref(&pkt);
                    break;
                }
            }
            
            // Initialize first DTS if not set
            if (first_video_dts == AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE) {
                first_video_dts = pkt.dts;
                first_video_pts = pkt.pts != AV_NOPTS_VALUE ? pkt.pts : pkt.dts;
                log_debug("First video DTS: %lld, PTS: %lld", 
                        (long long)first_video_dts, (long long)first_video_pts);
            }
            
            // Handle timestamps based on segment index
            if (segment_index == 0) {
                // First segment - adjust timestamps relative to first_dts
                if (pkt.dts != AV_NOPTS_VALUE && first_video_dts != AV_NOPTS_VALUE) {
                    pkt.dts -= first_video_dts;
                    if (pkt.dts < 0) pkt.dts = 0;
                }
                
                if (pkt.pts != AV_NOPTS_VALUE && first_video_pts != AV_NOPTS_VALUE) {
                    pkt.pts -= first_video_pts;
                    if (pkt.pts < 0) pkt.pts = 0;
                }
            } else {
                // Subsequent segments - maintain timestamp continuity
                // CRITICAL FIX: Use a small fixed offset instead of carrying over potentially large timestamps
                // This prevents the timestamp inflation issue while still maintaining continuity
                if (pkt.dts != AV_NOPTS_VALUE && first_video_dts != AV_NOPTS_VALUE) {
                    // Calculate relative timestamp within this segment
                    int64_t relative_dts = pkt.dts - first_video_dts;
                    // Add a small fixed offset (e.g., 1/30th of a second in timebase units)
                    // This ensures continuity without timestamp inflation
                    pkt.dts = relative_dts + 1;
                }
                
                if (pkt.pts != AV_NOPTS_VALUE && first_video_pts != AV_NOPTS_VALUE) {
                    int64_t relative_pts = pkt.pts - first_video_pts;
                    pkt.pts = relative_pts + 1;
                }
            }
            
            // CRITICAL FIX: Ensure PTS >= DTS for video packets to prevent "pts < dts" errors
            // This is essential for MP4 format compliance and prevents ghosting artifacts
            if (pkt.pts != AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE && pkt.pts < pkt.dts) {
                log_debug("Fixing video packet with PTS < DTS: PTS=%lld, DTS=%lld", 
                         (long long)pkt.pts, (long long)pkt.dts);
                pkt.pts = pkt.dts;
            }
            
            // Update last timestamps
            if (pkt.dts != AV_NOPTS_VALUE) {
                last_video_dts = pkt.dts;
            }
            if (pkt.pts != AV_NOPTS_VALUE) {
                last_video_pts = pkt.pts;
            }
            
            // Explicitly set duration to prevent segmentation fault during fragment writing
            // This addresses the "Estimating the duration of the last packet in a fragment" error
            if (pkt.duration == 0 || pkt.duration == AV_NOPTS_VALUE) {
                // Use the time base of the video stream to calculate a reasonable duration
                // For most video streams, this will be 1/framerate
                if (input_ctx->streams[video_stream_idx]->avg_frame_rate.num > 0 && 
                    input_ctx->streams[video_stream_idx]->avg_frame_rate.den > 0) {
                    // Calculate duration based on framerate (time_base units)
                    pkt.duration = av_rescale_q(1, 
                                               av_inv_q(input_ctx->streams[video_stream_idx]->avg_frame_rate),
                                               input_ctx->streams[video_stream_idx]->time_base);
                } else {
                    // Default to a reasonable value if framerate is not available
                    pkt.duration = 1;
                }
                log_debug("Set video packet duration to %lld", (long long)pkt.duration);
            }
            
            // Set output stream index
            pkt.stream_index = out_video_stream->index;
            
            // Write packet
            ret = av_interleaved_write_frame(output_ctx, &pkt);
            if (ret < 0) {
                log_error("Error writing video frame: %d", ret);
            } else {
                video_packet_count++;
                if (video_packet_count % 300 == 0) {
                    log_debug("Processed %d video packets", video_packet_count);
                }
            }
        }
        // Process audio packets - only if audio is enabled and we have an audio output stream
        else if (has_audio && audio_stream_idx >= 0 && pkt.stream_index == audio_stream_idx && out_audio_stream) {
            // Skip audio packets until we've found the first video keyframe
            if (!found_first_keyframe) {
                av_packet_unref(&pkt);
                continue;
            }
            
            // Initialize first audio DTS if not set
            if (first_audio_dts == AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE) {
                first_audio_dts = pkt.dts;
                first_audio_pts = pkt.pts != AV_NOPTS_VALUE ? pkt.pts : pkt.dts;
                log_debug("First audio DTS: %lld, PTS: %lld", 
                        (long long)first_audio_dts, (long long)first_audio_pts);
            }
            
            // Handle timestamps based on segment index
            if (segment_index == 0) {
                // First segment - adjust timestamps relative to first_dts
                if (pkt.dts != AV_NOPTS_VALUE && first_audio_dts != AV_NOPTS_VALUE) {
                    pkt.dts -= first_audio_dts;
                    if (pkt.dts < 0) pkt.dts = 0;
                }
                
                if (pkt.pts != AV_NOPTS_VALUE && first_audio_pts != AV_NOPTS_VALUE) {
                    pkt.pts -= first_audio_pts;
                    if (pkt.pts < 0) pkt.pts = 0;
                }
            } else {
                // Subsequent segments - maintain timestamp continuity
                // CRITICAL FIX: Use a small fixed offset instead of carrying over potentially large timestamps
                // This prevents the timestamp inflation issue while still maintaining continuity
                if (pkt.dts != AV_NOPTS_VALUE && first_audio_dts != AV_NOPTS_VALUE) {
                    // Calculate relative timestamp within this segment
                    int64_t relative_dts = pkt.dts - first_audio_dts;
                    // Add a small fixed offset (e.g., 1/30th of a second in timebase units)
                    // This ensures continuity without timestamp inflation
                    pkt.dts = relative_dts + 1;
                }
                
                if (pkt.pts != AV_NOPTS_VALUE && first_audio_pts != AV_NOPTS_VALUE) {
                    int64_t relative_pts = pkt.pts - first_audio_pts;
                    pkt.pts = relative_pts + 1;
                }
            }
            
            // Ensure monotonic increase of timestamps
            if (audio_packet_count > 0) {
                if (pkt.dts != AV_NOPTS_VALUE && pkt.dts <= last_audio_dts) {
                    pkt.dts = last_audio_dts + 1;
                }
                
                if (pkt.pts != AV_NOPTS_VALUE && pkt.pts <= last_audio_pts) {
                    pkt.pts = last_audio_pts + 1;
                }
                
                // Ensure PTS >= DTS
                if (pkt.pts != AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE && pkt.pts < pkt.dts) {
                    pkt.pts = pkt.dts;
                }
            }
            
            // CRITICAL FIX: Ensure DTS values don't exceed MP4 format limits (0x7fffffff) for audio packets
            if (pkt.dts != AV_NOPTS_VALUE) {
                if (pkt.dts > 0x7fffffff) {
                    log_warn("Audio DTS value exceeds MP4 format limit: %lld, resetting to safe value", (long long)pkt.dts);
                    pkt.dts = 1000;
                    if (pkt.pts != AV_NOPTS_VALUE) {
                        // Maintain PTS-DTS relationship if possible
                        int64_t pts_dts_diff = pkt.pts - pkt.dts;
                        if (pts_dts_diff >= 0) {
                            pkt.pts = pkt.dts + pts_dts_diff;
                        } else {
                            pkt.pts = pkt.dts;
                        }
                    } else {
                        pkt.pts = pkt.dts;
                    }
                }
                
                // Additional check to ensure DTS is always within safe range
                if (pkt.dts > 0x70000000) {  // ~75% of max value
                    log_info("Audio DTS value approaching MP4 format limit: %lld, resetting to prevent overflow", (long long)pkt.dts);
                    pkt.dts = 1000;
                    if (pkt.pts != AV_NOPTS_VALUE) {
                        // Maintain PTS-DTS relationship
                        pkt.pts = pkt.dts + 1;
                    } else {
                        pkt.pts = pkt.dts;
                    }
                }
            }
            
            // Update last timestamps
            if (pkt.dts != AV_NOPTS_VALUE) {
                last_audio_dts = pkt.dts;
            }
            if (pkt.pts != AV_NOPTS_VALUE) {
                last_audio_pts = pkt.pts;
            }
            
            // Explicitly set duration to prevent segmentation fault during fragment writing
            if (pkt.duration == 0 || pkt.duration == AV_NOPTS_VALUE) {
                // For audio, we can calculate duration based on sample rate and frame size
                AVStream *audio_stream = input_ctx->streams[audio_stream_idx];
                if (audio_stream->codecpar->sample_rate > 0) {
                    // If we know the number of samples in this packet, use that
                    int nb_samples = 0;
                    
                    // Try to get the number of samples from the codec parameters
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
                    // For FFmpeg 5.0 and newer
                    if (audio_stream->codecpar->ch_layout.nb_channels > 0 && 
                        audio_stream->codecpar->bits_per_coded_sample > 0) {
                        int bytes_per_sample = audio_stream->codecpar->bits_per_coded_sample / 8;
                        // Ensure we don't divide by zero
                        if (bytes_per_sample > 0) {
                            nb_samples = pkt.size / (audio_stream->codecpar->ch_layout.nb_channels * bytes_per_sample);
                        }
                    }
#else
                    // For older FFmpeg versions
                    if (audio_stream->codecpar->channels > 0 && 
                        audio_stream->codecpar->bits_per_coded_sample > 0) {
                        int bytes_per_sample = audio_stream->codecpar->bits_per_coded_sample / 8;
                        // Ensure we don't divide by zero
                        if (bytes_per_sample > 0) {
                            nb_samples = pkt.size / (audio_stream->codecpar->channels * bytes_per_sample);
                        }
                    }
#endif
                    
                    if (nb_samples > 0) {
                        // Calculate duration based on samples and sample rate
                        pkt.duration = av_rescale_q(nb_samples, 
                                                  (AVRational){1, audio_stream->codecpar->sample_rate},
                                                  audio_stream->time_base);
                    } else {
                        // Default to a reasonable value based on sample rate
                        // Typically audio frames are ~20-40ms, so we'll use 1024 samples as a common value
                        pkt.duration = av_rescale_q(1024, 
                                                  (AVRational){1, audio_stream->codecpar->sample_rate},
                                                  audio_stream->time_base);
                    }
                } else {
                    // If we can't calculate based on sample rate, use a default value
                    pkt.duration = 1;
                    log_debug("Set default audio packet duration to 1");
                }
            }
            
            // Set output stream index
            pkt.stream_index = out_audio_stream->index;
            
            // Write packet
            ret = av_interleaved_write_frame(output_ctx, &pkt);
            if (ret < 0) {
                log_error("Error writing audio frame: %d", ret);
            } else {
                audio_packet_count++;
                if (audio_packet_count % 300 == 0) {
                    log_debug("Processed %d audio packets", audio_packet_count);
                }
            }
        }
        
        // Unref packet
        av_packet_unref(&pkt);
    }
    
    log_info("Recording segment complete (video packets: %d, audio packets: %d)", 
            video_packet_count, audio_packet_count);
            
    // Flag to track if trailer has been written
    bool trailer_written = false;
    
    // Write trailer
    if (output_ctx && output_ctx->pb) {
        ret = av_write_trailer(output_ctx);
        if (ret < 0) {
            log_error("Failed to write trailer: %d", ret);
        } else {
            trailer_written = true;
            log_debug("Successfully wrote trailer to output file");
        }
    }
    
    // Save segment info for the next segment if needed
    if (prev_segment_info) {
        prev_segment_info->segment_index = segment_index;
        prev_segment_info->has_audio = has_audio && audio_stream_idx >= 0;
        log_debug("Saved segment info for next segment: index=%d, has_audio=%d",
                segment_index, has_audio && audio_stream_idx >= 0);
    }
    
cleanup:
    // CRITICAL FIX: Minimal cleanup to avoid double free issues
    // Only clean up what we know is safe
    
    // Free dictionaries - these are always safe to free
    av_dict_free(&opts);
    av_dict_free(&out_opts);
    
    // Only clean up output context if it was successfully created
    if (output_ctx) {
        // Only write trailer if we successfully wrote the header
        if (output_ctx->pb && ret >= 0 && !trailer_written) {
            av_write_trailer(output_ctx);
        }
        
        // Close output file if it was opened
        if (output_ctx->pb) {
            avio_closep(&output_ctx->pb);
        }
        
        // Free output context
        avformat_free_context(output_ctx);
    }
    
    // IMPORTANT: Do not touch input_ctx here - it's managed by the caller
    // The caller will reuse it for the next segment or close it when done
    
    // Return the error code
    
    return ret;
}

/**
 * RTSP stream reading thread function
 * This function maintains a single RTSP connection across multiple segments
 */
static void *mp4_writer_rtsp_thread(void *arg) {
    mp4_writer_thread_t *thread_ctx = (mp4_writer_thread_t *)arg;
    AVFormatContext *input_ctx = NULL;
    AVPacket *pkt = NULL;
    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    int ret;
    time_t start_time = time(NULL);  // Record when we started
    segment_info_t segment_info = {0};  // Initialize segment info for timestamp continuity
    
    // Make a local copy of the stream name for thread safety
    char stream_name[MAX_STREAM_NAME];
    if (thread_ctx->writer && thread_ctx->writer->stream_name[0] != '\0') {
        strncpy(stream_name, thread_ctx->writer->stream_name, MAX_STREAM_NAME - 1);
        stream_name[MAX_STREAM_NAME - 1] = '\0';
    } else {
        strncpy(stream_name, "unknown", MAX_STREAM_NAME - 1);
    }

    log_info("Starting RTSP reading thread for stream %s", stream_name);
    
    // Add initial recording metadata to the database
    if (thread_ctx->writer && thread_ctx->writer->output_path[0] != '\0') {
        recording_metadata_t metadata;
        memset(&metadata, 0, sizeof(recording_metadata_t));
        
        // Fill in the metadata
        strncpy(metadata.stream_name, stream_name, sizeof(metadata.stream_name) - 1);
        strncpy(metadata.file_path, thread_ctx->writer->output_path, sizeof(metadata.file_path) - 1);
        metadata.start_time = start_time;
        metadata.end_time = 0; // Will be updated when recording ends
        metadata.size_bytes = 0; // Will be updated as recording grows
        metadata.is_complete = false;
        
        // Add recording to database
        uint64_t recording_id = add_recording_metadata(&metadata);
        if (recording_id == 0) {
            log_error("Failed to add initial recording metadata for stream %s", stream_name);
        } else {
            log_info("Added initial recording to database with ID: %llu for file: %s", 
                    (unsigned long long)recording_id, thread_ctx->writer->output_path);
            
            // Store the recording ID in the writer for later update
            thread_ctx->writer->current_recording_id = recording_id;
        }
    }

    // Check if we're still running (might have been stopped during initialization)
    if (!thread_ctx->running || thread_ctx->shutdown_requested) {
        log_info("RTSP reading thread for %s exiting early due to shutdown", stream_name);
        return NULL;
    }

    // Initialize segment info
    segment_info.segment_index = 0;
    segment_info.has_audio = false;
    segment_info.last_frame_was_key = false;

    // Main loop to record segments
    while (thread_ctx->running && !thread_ctx->shutdown_requested) {
        // Check if shutdown has been initiated
        if (is_shutdown_initiated()) {
            log_info("RTSP reading thread for %s stopping due to system shutdown", stream_name);
            thread_ctx->running = 0;
            break;
        }
        
        // Get current time
        time_t current_time = time(NULL);
        
        // Fetch the latest stream configuration from the database
        stream_config_t db_stream_config;
        int db_config_result = get_stream_config_by_name(stream_name, &db_stream_config);
        
        // Define segment_duration variable outside the if block
        int segment_duration = thread_ctx->writer->segment_duration;
        
        // Update configuration from database if available
        if (db_config_result == 0) {
            // Update segment duration if available
            if (db_stream_config.segment_duration > 0) {
                segment_duration = db_stream_config.segment_duration;
                
                // Update the writer's segment duration if it has changed
                if (thread_ctx->writer->segment_duration != segment_duration) {
                    log_info("Updating segment duration for stream %s from %d to %d seconds (from database)",
                            stream_name, thread_ctx->writer->segment_duration, segment_duration);
                    thread_ctx->writer->segment_duration = segment_duration;
                }
            }
            
            // Update audio recording setting if it has changed
            int has_audio = db_stream_config.record_audio ? 1 : 0;
            if (thread_ctx->writer->has_audio != has_audio) {
                log_info("Updating audio recording setting for stream %s from %s to %s (from database)",
                        stream_name, 
                        thread_ctx->writer->has_audio ? "enabled" : "disabled",
                        has_audio ? "enabled" : "disabled");
                thread_ctx->writer->has_audio = has_audio;
            }
        }
        
        // Check if it's time to create a new segment based on segment duration
        // Force segment rotation every segment_duration seconds
        if (segment_duration > 0) {
            time_t elapsed_time = current_time - thread_ctx->writer->last_rotation_time;
            if (elapsed_time >= segment_duration) {
                log_info("Time to create new segment for stream %s (elapsed time: %ld seconds, segment duration: %d seconds)", 
                         stream_name, (long)elapsed_time, segment_duration);
                
                // Create timestamp for new MP4 filename
                char timestamp_str[32];
                struct tm *tm_info = localtime(&current_time);
                strftime(timestamp_str, sizeof(timestamp_str), "%Y%m%d_%H%M%S", tm_info);
                
                // Create new output path
                char new_path[MAX_PATH_LENGTH];
                snprintf(new_path, MAX_PATH_LENGTH, "%s/recording_%s.mp4",
                         thread_ctx->writer->output_dir, timestamp_str);
                
                // Get the current output path before closing
                char current_path[MAX_PATH_LENGTH];
                strncpy(current_path, thread_ctx->writer->output_path, MAX_PATH_LENGTH - 1);
                current_path[MAX_PATH_LENGTH - 1] = '\0';
                
                // Create recording metadata for the new file
                recording_metadata_t metadata;
                memset(&metadata, 0, sizeof(recording_metadata_t));
                
                // Fill in the metadata
                strncpy(metadata.stream_name, stream_name, sizeof(metadata.stream_name) - 1);
                strncpy(metadata.file_path, new_path, sizeof(metadata.file_path) - 1);
                metadata.start_time = current_time;
                metadata.end_time = 0; // Will be updated when recording ends
                metadata.size_bytes = 0; // Will be updated as recording grows
                metadata.is_complete = false;
                
                // Add recording to database for the new file
                uint64_t new_recording_id = add_recording_metadata(&metadata);
                if (new_recording_id == 0) {
                    log_error("Failed to add recording metadata for stream %s during rotation", stream_name);
                } else {
                    log_info("Added new recording to database with ID: %llu for rotated file: %s", 
                            (unsigned long long)new_recording_id, new_path);
                }
                
                // Mark the previous recording as complete
                if (thread_ctx->writer->current_recording_id > 0) {
                    // Get the file size before marking as complete
                    struct stat st;
                    uint64_t size_bytes = 0;
                    
                    if (stat(current_path, &st) == 0) {
                        size_bytes = st.st_size;
                        log_info("File size for %s: %llu bytes", 
                                current_path, (unsigned long long)size_bytes);
                        
                        // Mark the recording as complete with the correct file size
                        update_recording_metadata(thread_ctx->writer->current_recording_id, current_time, size_bytes, true);
                        log_info("Marked previous recording (ID: %llu) as complete for stream %s (size: %llu bytes)", 
                                (unsigned long long)thread_ctx->writer->current_recording_id, stream_name, (unsigned long long)size_bytes);
                    } else {
                        log_warn("Failed to get file size for %s: %s", 
                                current_path, strerror(errno));
                        
                        // Still mark the recording as complete, but with size 0
                        update_recording_metadata(thread_ctx->writer->current_recording_id, current_time, 0, true);
                        log_info("Marked previous recording (ID: %llu) as complete for stream %s (size unknown)", 
                                (unsigned long long)thread_ctx->writer->current_recording_id, stream_name);
                    }
                }
                
                // Update the output path
                strncpy(thread_ctx->writer->output_path, new_path, MAX_PATH_LENGTH - 1);
                thread_ctx->writer->output_path[MAX_PATH_LENGTH - 1] = '\0';
                
                // Store the new recording ID in the writer for later update
                if (new_recording_id > 0) {
                    thread_ctx->writer->current_recording_id = new_recording_id;
                }
                
                // Update rotation time
                thread_ctx->writer->last_rotation_time = current_time;
            }
        }
        
        // Record a segment using the record_segment function
        log_info("Recording segment for stream %s to %s", stream_name, thread_ctx->writer->output_path);
        
        // Use the segment duration from the database or writer
        if (segment_duration > 0) {
            log_info("Using segment duration: %d seconds (from %s)", 
                    segment_duration, 
                    (db_config_result == 0 && db_stream_config.segment_duration > 0) ? "database" : "writer context");
        } else {
            segment_duration = 30;
            log_info("No segment duration configured, using default: %d seconds", segment_duration);
        }
        
        // Variables for retry mechanism
        static int segment_retry_count = 0;
        static time_t last_segment_retry_time = 0;
        
        // Record the segment with timestamp continuity
        ret = record_segment(thread_ctx->rtsp_url, thread_ctx->writer->output_path, 
                           segment_duration, &input_ctx, thread_ctx->writer->has_audio, &segment_info);
        
        if (ret < 0) {
            log_error("Failed to record segment for stream %s (error: %d), implementing retry strategy...", 
                     stream_name, ret);
                     
            // Check if input_ctx is NULL after a failed record_segment call
            // This can happen if the connection failed and avformat_open_input failed
            if (input_ctx == NULL) {
                log_warn("Input context is NULL after record_segment failure for stream %s", stream_name);
            }
            
            // Calculate backoff time based on retry count (exponential backoff with max of 30 seconds)
            int backoff_seconds = 1 << (segment_retry_count > 4 ? 4 : segment_retry_count); // 1, 2, 4, 8, 16, 16, ...
            if (backoff_seconds > 30) backoff_seconds = 30;
            
            // Record the retry attempt
            segment_retry_count++;
            last_segment_retry_time = time(NULL);
            
            // If input context was closed, set it to NULL so it will be reopened
            if (!input_ctx) {
                log_info("Input context was closed, will reopen on next attempt");
            }
            
            // If we've had too many consecutive failures, try more aggressive recovery
            if (segment_retry_count > 5) {
                log_warn("Multiple segment recording failures for %s (%d retries), attempting aggressive recovery", 
                        stream_name, segment_retry_count);
                
                // Force input context to be recreated
                if (input_ctx) {
                    avformat_close_input(&input_ctx);
                    input_ctx = NULL;
                    log_info("Forcibly closed input context to ensure fresh connection on next attempt");
                }
                
                // Sleep longer for aggressive recovery
                backoff_seconds = 5;
            }
            
            log_info("Waiting %d seconds before retrying segment recording for %s (retry #%d)", 
                    backoff_seconds, stream_name, segment_retry_count);
            
            // Wait before trying again
            av_usleep(backoff_seconds * 1000000);  // Convert to microseconds
        } else {
            // Reset retry count on success
            if (segment_retry_count > 0) {
                log_info("Successfully recorded segment for %s after %d retries", 
                        stream_name, segment_retry_count);
                segment_retry_count = 0;
            }
        }
        
        // Update the last packet time for activity tracking
        thread_ctx->writer->last_packet_time = time(NULL);
        
        // Update the recording metadata with the current file size
        if (thread_ctx->writer->current_recording_id > 0) {
            struct stat st;
            if (stat(thread_ctx->writer->output_path, &st) == 0) {
                uint64_t size_bytes = st.st_size;
                // Update size but don't mark as complete yet
                update_recording_metadata(thread_ctx->writer->current_recording_id, 0, size_bytes, false);
                log_debug("Updated recording metadata for ID: %llu, size: %llu bytes", 
                        (unsigned long long)thread_ctx->writer->current_recording_id, 
                        (unsigned long long)size_bytes);
            }
        }
    }

    // Clean up resources
    if (pkt) {
        // Make a local copy of the packet pointer and NULL out the original
        // to prevent double-free if another thread accesses it
        AVPacket *pkt_to_free = pkt;
        pkt = NULL;
        
        // Now safely free the packet - first unref then free to prevent memory leaks
        av_packet_unref(pkt_to_free);
        av_packet_free(&pkt_to_free);
    }
    
    if (input_ctx) {
        // Make a local copy of the context pointer and NULL out the original
        AVFormatContext *ctx_to_close = input_ctx;
        input_ctx = NULL;
        
        // Now safely close the input context
        avformat_close_input(&ctx_to_close);
    }

    log_info("RTSP reading thread for stream %s exited", stream_name);
    return NULL;
}

/**
 * Start a recording thread that reads from the RTSP stream and writes to the MP4 file
 * This function creates a new thread that handles all the recording logic
 */
int mp4_writer_start_recording_thread(mp4_writer_t *writer, const char *rtsp_url) {
    if (!writer || !rtsp_url || rtsp_url[0] == '\0') {
        log_error("Invalid parameters passed to mp4_writer_start_recording_thread");
        return -1;
    }
    
    // Create thread context
    mp4_writer_thread_t *thread_ctx = calloc(1, sizeof(mp4_writer_thread_t));
    if (!thread_ctx) {
        log_error("Failed to allocate memory for thread context");
        return -1;
    }
    
    // Initialize thread context
    thread_ctx->running = 1;
    thread_ctx->shutdown_requested = 0;
    thread_ctx->writer = writer;
    strncpy(thread_ctx->rtsp_url, rtsp_url, MAX_PATH_LENGTH - 1);
    
    // Create thread
    if (pthread_create(&thread_ctx->thread, NULL, mp4_writer_rtsp_thread, thread_ctx) != 0) {
        log_error("Failed to create RTSP reading thread for %s", writer->stream_name);
        free(thread_ctx);
        return -1;
    }
    
    // Store thread context in writer
    writer->thread_ctx = thread_ctx;
    
    // Register with shutdown coordinator
    writer->shutdown_component_id = register_component(
        writer->stream_name, 
        COMPONENT_MP4_WRITER, 
        writer, 
        10  // Medium priority
    );
    
    if (writer->shutdown_component_id >= 0) {
        log_info("Registered MP4 writer for %s with shutdown coordinator, component ID: %d", 
                writer->stream_name, writer->shutdown_component_id);
    } else {
        log_warn("Failed to register MP4 writer for %s with shutdown coordinator", writer->stream_name);
    }
    
    log_info("Started RTSP reading thread for %s", writer->stream_name);
    
    return 0;
}

/**
 * Stop the recording thread
 * This function signals the recording thread to stop and waits for it to exit
 */
void mp4_writer_stop_recording_thread(mp4_writer_t *writer) {
    if (!writer) {
        log_warn("NULL writer passed to mp4_writer_stop_recording_thread");
        return;
    }
    
    mp4_writer_thread_t *thread_ctx = (mp4_writer_thread_t *)writer->thread_ctx;
    if (!thread_ctx) {
        log_warn("No thread context found for writer %s", writer->stream_name ? writer->stream_name : "unknown");
        return;
    }
    
    // Make a local copy of the stream name for logging
    char stream_name[MAX_STREAM_NAME];
    if (writer->stream_name && writer->stream_name[0] != '\0') {
        strncpy(stream_name, writer->stream_name, MAX_STREAM_NAME - 1);
        stream_name[MAX_STREAM_NAME - 1] = '\0';
    } else {
        strncpy(stream_name, "unknown", MAX_STREAM_NAME - 1);
    }
    
    log_info("Signaling RTSP reading thread for %s to stop", stream_name);
    
    // Signal thread to stop
    thread_ctx->running = 0;
    thread_ctx->shutdown_requested = 1;
    
    // Wait for thread to exit with timeout
    #include "video/thread_utils.h"
    int join_result = pthread_join_with_timeout(thread_ctx->thread, NULL, 5);
    if (join_result != 0) {
        log_warn("Failed to join RTSP reading thread for %s within timeout: %s", 
                stream_name, strerror(join_result));
        
        // Don't free the thread context if join failed
        // Instead, detach the thread to let it clean up itself when it eventually exits
        pthread_detach(thread_ctx->thread);
        
        // Set the thread context pointer to NULL to prevent further access
        // but don't free it as the thread might still be using it
        writer->thread_ctx = NULL;
        
        log_info("Detached RTSP reading thread for %s to prevent memory corruption", stream_name);
    } else {
        log_info("Successfully joined RTSP reading thread for %s", stream_name);
        
        // Free thread context only after successful join
        free(thread_ctx);
        writer->thread_ctx = NULL;
    }
    
    // Ensure we update the component state even if join failed
    
    // Update component state in shutdown coordinator
    if (writer->shutdown_component_id >= 0) {
        update_component_state(writer->shutdown_component_id, COMPONENT_STOPPED);
        log_info("Updated MP4 writer component state to STOPPED for %s", stream_name);
    }
    
    log_info("Stopped RTSP reading thread for %s", stream_name);
}

/**
 * Check if the recording thread is running
 */
int mp4_writer_is_recording(mp4_writer_t *writer) {
    if (!writer) {
        return 0;
    }
    
    // If the writer is in the process of rotating, consider it as still recording
    if (writer->is_rotating) {
        return 1;
    }
    
    mp4_writer_thread_t *thread_ctx = (mp4_writer_thread_t *)writer->thread_ctx;
    if (!thread_ctx) {
        return 0;
    }
    
    return thread_ctx->running;
}
