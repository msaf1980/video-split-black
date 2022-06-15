#define _XOPEN_SOURCE 600 /* for usleep */
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>

#include <libgen.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kstring.h"

/* Split video file with black frame delimeters */

const char *filter_descr = "blackdetect=d=1:pix_th=0.00";
/* other way:
   scale=78:24 [scl]; [scl] transpose=cclock // assumes "[in]" and "[out]" to be
   input output pads respectively
 */
static AVFormatContext *fmt_ctx;
static AVCodecContext * dec_ctx;
AVFilterContext *       buffersink_ctx;
AVFilterContext *       buffersrc_ctx;
AVFilterGraph *         filter_graph;
static int              video_stream_index = -1;
static int64_t          last_pts = AV_NOPTS_VALUE;

static int open_input_file(const char *filename) {
	int      ret;
	AVCodec *dec;
	if ((ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
		return ret;
	}

    // Dump information about file onto standard error
	av_dump_format(fmt_ctx, 0, filename, 0);

	if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
		return ret;
	}
	/* select the video stream */
	ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR,
		       "Cannot find a video stream in the input file\n");
		return ret;
	}
	video_stream_index = ret;
	/* create decoding context */
	dec_ctx = avcodec_alloc_context3(dec);
	if (!dec_ctx)
		return AVERROR(ENOMEM);
	avcodec_parameters_to_context(
	    dec_ctx, fmt_ctx->streams[video_stream_index]->codecpar);
	av_opt_set_int(dec_ctx, "refcounted_frames", 1, 0);
	/* init the video decoder */
	if ((ret = avcodec_open2(dec_ctx, dec, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot open video decoder\n");
		return ret;
	}
	return 0;
}

static int init_filters(const char *filters_descr) {
	char            args[512];
	int             ret = 0;
	const AVFilter *buffersrc = avfilter_get_by_name("buffer");
	const AVFilter *buffersink = avfilter_get_by_name("buffersink");
	AVFilterInOut * outputs = avfilter_inout_alloc();
	AVFilterInOut * inputs = avfilter_inout_alloc();
	AVRational      time_base = fmt_ctx->streams[video_stream_index]->time_base;
	enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE};
	filter_graph = avfilter_graph_alloc();
	if (!outputs || !inputs || !filter_graph) {
		ret = AVERROR(ENOMEM);
		goto EXIT;
	}
	/* buffer video source: the decoded frames from the decoder will be inserted
	 * here. */
	snprintf(args, sizeof(args),
	         "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
	         dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt, time_base.num,
	         time_base.den, dec_ctx->sample_aspect_ratio.num,
	         dec_ctx->sample_aspect_ratio.den);
	ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args,
	                                   NULL, filter_graph);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
		goto EXIT;
	}
	/* buffer video sink: to terminate the filter chain. */
	ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL,
	                                   NULL, filter_graph);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
		goto EXIT;
	}
	ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts,
	                          AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
		goto EXIT;
	}
	/*
	 * Set the endpoints for the filter graph. The filter_graph will
	 * be linked to the graph described by filters_descr.
	 */
	/*
	 * The buffer source output must be connected to the input pad of
	 * the first filter described by filters_descr; since the first
	 * filter input label is not specified, it is set to "in" by
	 * default.
	 */
	outputs->name = av_strdup("in");
	outputs->filter_ctx = buffersrc_ctx;
	outputs->pad_idx = 0;
	outputs->next = NULL;
	/*
	 * The buffer sink input must be connected to the output pad of
	 * the last filter described by filters_descr; since the last
	 * filter output label is not specified, it is set to "out" by
	 * default.
	 */
	inputs->name = av_strdup("out");
	inputs->filter_ctx = buffersink_ctx;
	inputs->pad_idx = 0;
	inputs->next = NULL;
	if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr, &inputs,
	                                    &outputs, NULL)) < 0)
		goto EXIT;
	if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
		goto EXIT;
EXIT:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	return ret;
}

static void display_frame(const AVFrame *frame, AVRational time_base) {
	int      x, y;
	uint8_t *p0, *p;
	int64_t  delay;
	if (frame->pts != AV_NOPTS_VALUE) {
		if (last_pts != AV_NOPTS_VALUE) {
			/* sleep roughly the right amount of time;
			 * usleep is in microseconds, just like AV_TIME_BASE. */
			delay =
			    av_rescale_q(frame->pts - last_pts, time_base, AV_TIME_BASE_Q);
			if (delay > 0 && delay < 1000000)
				usleep(delay);
		}
		last_pts = frame->pts;
	}
	/* Trivial ASCII grayscale display. */
	p0 = frame->data[0];
	puts("\033c");
	for (y = 0; y < frame->height; y++) {
		p = p0;
		for (x = 0; x < frame->width; x++)
			putchar(" .-+#"[*(p++) / 52]);
		putchar('\n');
		p0 += frame->linesize[0];
	}
	fflush(stdout);
}

char *strip_ext(char *fname) {
	char *end = fname + strlen(fname);

	while (end > fname && *end != '.' && *end != '\\' && *end != '/') {
		--end;
	}
	if ((end > fname && *end == '.') &&
	    (*(end - 1) != '\\' && *(end - 1) != '/')) {
		*end = '\0';
		return end + 1;
	}
	return NULL;
}

int main(int argc, char **argv) {
	kstring_t segments = {0, 0, NULL};

	int                ret;
	AVDictionaryEntry *tag = NULL;
	AVPacket           packet;
	AVFrame *          frame = av_frame_alloc();
	AVFrame *          filt_frame = av_frame_alloc();
	if (!frame || !filt_frame) {
		perror("Could not allocate frame");
		exit(1);
	}
	if (argc != 3) {
		fprintf(stderr, "Split video file with black frame delimeters\nUsage: %s input_file output_dir\n", argv[0]);
		exit(1);
	}

	char *in_file = argv[1];
	char *out_dir = argv[2];

	if ((ret = open_input_file(in_file)) < 0)
		goto EXIT;
	if ((ret = init_filters(filter_descr)) < 0)
		goto EXIT;

    int64_t duration = fmt_ctx->duration;
    fprintf(stderr, "Try to detect black frames\n");

	/* read all packets */
	while (1) {
		if ((ret = av_read_frame(fmt_ctx, &packet)) < 0)
			break;
		if (packet.stream_index == video_stream_index) {
			ret = avcodec_send_packet(dec_ctx, &packet);
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR,
				       "Error while sending a packet to the decoder\n");
				break;
			}
			while (ret >= 0) {
				ret = avcodec_receive_frame(dec_ctx, frame);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
					break;
				} else if (ret < 0) {
					av_log(NULL, AV_LOG_ERROR,
					       "Error while receiving a frame from the decoder\n");
					goto EXIT;
				}
				if (ret >= 0) {
					frame->pts = frame->best_effort_timestamp;
					/* push the decoded frame into the filtergraph */
					if (av_buffersrc_add_frame_flags(
					        buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) <
					    0) {
						av_log(NULL, AV_LOG_ERROR,
						       "Error while feeding the filtergraph\n");
						break;
					}
					/* pull filtered frames from the filtergraph */
					while (1) {
						ret =
						    av_buffersink_get_frame(buffersink_ctx, filt_frame);
						if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
							break;
						if (ret < 0)
							goto EXIT;
						// display_frame(filt_frame,
						// buffersink_ctx->inputs[0]->time_base);
						if ((tag = av_dict_get(filt_frame->metadata,
						                       "lavfi.black_start", tag,
						                       AV_DICT_IGNORE_SUFFIX))) {
							printf("%s = %s (%.2f%%)\n", tag->key, tag->value, AV_TIME_BASE*100*atof(tag->value)/duration);
							if (ks_len(&segments) != 0) {
								if (kputc(',', &segments) == EOF) {
									av_log(NULL, AV_LOG_ERROR,
									       "Error append to segment\n");
								}
							}
							kputs(tag->value, &segments);
						}
						if ((tag = av_dict_get(filt_frame->metadata,
						                       "lavfi.black_end", tag,
						                       AV_DICT_IGNORE_SUFFIX))) {
							printf("%s = %s\n", tag->key, tag->value);
						}
						av_frame_unref(filt_frame);
					}
					av_frame_unref(frame);
				}
			}
		}
		av_packet_unref(&packet);
	}
EXIT:
	avfilter_graph_free(&filter_graph);
	avcodec_free_context(&dec_ctx);
	avformat_close_input(&fmt_ctx);
	av_frame_free(&frame);
	av_frame_free(&filt_frame);
	if (ret < 0 && ret != AVERROR_EOF) {
		fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
		exit(1);
	}
	if (ks_len(&segments) > 0) {
		char *name = strdup(basename((char *) in_file));
		char *ext = strip_ext(name);

		kstring_t split_cmd = {0, 0, NULL};
		ksprintf(&split_cmd,
		         "ffmpeg -i '%s' -f segment -segment_times %s -c copy "
		         "-reset_timestamps 1 -map 0 '%s/%s-%%d.%s'",
		         in_file, segments.s, out_dir, name, ext);
		free(name);

		printf("RUN SPLIT: %s\n", split_cmd.s);
		mkdir(out_dir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
		return system(split_cmd.s);
	}
	exit(0);
}