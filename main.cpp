#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>
#include <boost/scope_exit.hpp>
#include <cstring>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <rtc/track.hpp>
#include <rtc/configuration.hpp>
#include <rtc/datachannel.hpp>
#include <rtc/global.hpp>
#include <rtc/h264rtpdepacketizer.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/peerconnection.hpp>
#include <rtc/rtc.h>
#include <rtc/rtcpnackresponder.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/track.hpp>
#include <rtc/utils.hpp>
#include <rtc/websocket.hpp>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock.h>
#else
#include <arpa/inet.h>
#endif

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

const uint8_t H264_START_SEQUENCE[] = { 0, 0, 0, 1 };
std::atomic<bool> running = true;

#ifdef __linux__
void sig(const int signum)
{
  running = false;
}
#endif

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type)
{
  switch (ctrl_type)
  {
    case CTRL_C_EVENT:
    {
      running = false;
      return TRUE;
    }
    default:
    {
      return FALSE;
    }
  }
}

struct Connection
{
  Connection(const std::string& filename)
    : filename_(filename)
    , peer_connection_(std::make_shared<rtc::PeerConnection>())
  {
  }

  ~Connection()
  {
    if (thread_.joinable())
    {
      thread_.join();
    }
  }

  void Start()
  {
    thread_ = std::thread([this]()
                          {
                            // Open the file
                            std::cout << "Opening the file: " << filename_ << std::endl;
                            AVFormatContext* format_context = nullptr;
                            if (avformat_open_input(&format_context, filename_.c_str(), nullptr, nullptr) != 0)
                            {
                              std::cout << "Failed to open avformat file: " << filename_ << std::endl;
                              return;
                            }
                            BOOST_SCOPE_EXIT(format_context)
                            {
                              avformat_close_input(&format_context);
                            }
                            BOOST_SCOPE_EXIT_END
                              if (avformat_find_stream_info(format_context, nullptr) < 0)
                              {
                                std::cout << "Failed to find stream info: " << filename_ << std::endl;
                                return;
                              }
                            std::optional<unsigned int> video_stream;
                            for (unsigned int i = 0; i < format_context->nb_streams; i++)
                            {
                              if ((format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) && (format_context->streams[i]->codecpar->codec_id == AVCodecID::AV_CODEC_ID_H264)) // Currently only support H264
                              {
                                video_stream = i;
                                break;
                              }
                            }
                            if (!video_stream.has_value())
                            {
                              std::cout << "Failed to find video stream: " << filename_ << std::endl;
                              return;
                            }
                            // Find SPS/PPS if available and pass it to the decoder
                            std::vector<uint8_t> sps_pps;
                            if (format_context->streams[*video_stream]->codecpar->extradata && format_context->streams[*video_stream]->codecpar->extradata_size)
                            {
                              const std::vector<uint8_t> extradata(format_context->streams[*video_stream]->codecpar->extradata, format_context->streams[*video_stream]->codecpar->extradata + format_context->streams[*video_stream]->codecpar->extradata_size);
                              if (extradata.size())
                              {
                                if (extradata[0] >= 1) // SPS+PPS count, but we only care about the first one
                                {
                                  const int spscount = extradata[5] & 0x1f;
                                  const int spsnalsize = (extradata[6] << 8) | extradata[7];
                                  if ((spsnalsize + 8) <= extradata.size())
                                  {
                                    std::cout << "Gathering SPS: " << spsnalsize << std::endl;
                                    sps_pps.insert(sps_pps.end(), H264_START_SEQUENCE, H264_START_SEQUENCE + sizeof(H264_START_SEQUENCE));
                                    sps_pps.insert(sps_pps.end(), extradata.data() + 8, extradata.data() + 8 + spsnalsize);
                                    if ((spsnalsize + 8 + 1) <= extradata.size())
                                    {
                                      const int ppscount = extradata[8 + spsnalsize] & 0x1f;
                                      if (ppscount >= 1)
                                      {
                                        if ((spsnalsize + 8 + 1 + 2) < extradata.size())
                                        {
                                          const int ppsnalsize = (extradata[8 + spsnalsize + 1] << 8) | extradata[8 + spsnalsize + 2];
                                          if ((spsnalsize + 8 + 1 + 2 + ppsnalsize) <= extradata.size())
                                          {
                                            std::cout << "Gathering PPS: " << ppsnalsize << std::endl;
                                            sps_pps.insert(sps_pps.end(), H264_START_SEQUENCE, H264_START_SEQUENCE + sizeof(H264_START_SEQUENCE));
                                            sps_pps.insert(sps_pps.end(), extradata.data() + 8 + spsnalsize + 3, extradata.data() + 8 + spsnalsize + 3 + ppsnalsize);
                                          }
                                        }
                                      }
                                    }
                                  }
                                }
                              }
                            }
                            // Main loop
                            std::cout << "Starting main loop" << std::endl;
                            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
                            static double time_base = static_cast<double>(format_context->streams[*video_stream]->time_base.num) / static_cast<double>(format_context->streams[*video_stream]->time_base.den);
                            AVPacket* av_packet = nullptr;
                            BOOST_SCOPE_EXIT(&av_packet)
                            {
                              if (av_packet)
                              {
                                av_packet_free(&av_packet);
                              }
                            }
                            BOOST_SCOPE_EXIT_END
                            // Start main loop
                            std::vector<uint8_t> buf;
                            uint64_t prev_video_timestamp = 0;
                            while (running)
                            {
                              // Calculate time of frame, we spin on this and free the packet when we are ready for the next packet
                              if (av_packet)
                              {
                                const double av_packet_time = static_cast<double>(av_packet->pts) * time_base;
                                const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
                                if (now >= (start + std::chrono::milliseconds(static_cast<int>(av_packet_time * 1000.0))))
                                {
                                  av_packet_free(&av_packet);
                                }
                                std::this_thread::yield();
                              }
                              // Read frame from file
                              if (av_packet == nullptr)
                              {
                                av_packet = av_packet_alloc();
                                int ret = av_read_frame(format_context, av_packet);
                                if (ret == AVERROR_EOF)
                                {
                                  ret = av_seek_frame(format_context, *video_stream, 0, AVSEEK_FLAG_ANY);
                                  if (ret < 0)
                                  {
                                    std::cout << "Failed to seek frame" << std::endl;
                                    return;
                                  }
                                  start = std::chrono::steady_clock::now();
                                }
                                else if (ret)
                                {
                                  std::cout << "Failed to seek frame" << std::endl;
                                  return;
                                }
                                // Video
                                if (av_packet->stream_index != *video_stream)
                                {
                                  av_packet_free(&av_packet);
                                  continue;
                                }
                                // Do we need to insert SPS/PPS?
                                bool insert_sps = false;
                                const uint8_t* ptr = av_packet->data;
                                size_t size = av_packet->size;
                                if (sps_pps.size())
                                {
                                  bool idr_slice = false;
                                  bool sps = false;
                                  bool pps = false;
                                  while (size > 5)
                                  {
                                    const uint32_t nal_size = htonl(*reinterpret_cast<const uint32_t*>(ptr));
                                    ptr += 4;
                                    size -= 4;
                                    if (nal_size > size)
                                    {
                                      std::cout << "Illegal NAL size " << nal_size << std::endl;
                                      break;
                                    }
                                    const uint8_t naltype = *ptr & 0x1f;
                                    if (naltype == 5)
                                    {
                                      idr_slice = true;
                                      break;
                                    }
                                    else if (naltype == 7)
                                    {
                                      sps = true;
                                    }
                                    else if (naltype == 8)
                                    {
                                      pps = true;
                                    }
                                    ptr += nal_size;
                                    size -= nal_size;
                                  }
                                  if (idr_slice && (sps == false) && (pps == false)) // If we are an iframe and we don't have an SPS or PPS before the IDR, then insert the sprop-parameter-set extra adata SPS and PPS
                                  {
                                    insert_sps = true;
                                  }
                                }
                                // Send
                                ptr = av_packet->data;
                                size = av_packet->size;
                                while (size > 5)
                                {
                                  const uint32_t nal_size = htonl(*reinterpret_cast<const uint32_t*>(ptr));
                                  ptr += 4;
                                  size -= 4;
                                  if (nal_size > size)
                                  {
                                    std::cout << "Illegal NAL size " << nal_size << std::endl;
                                    break;
                                  }
                                  buf.clear();
                                  if (insert_sps) // We should always have extra data, but if for some reason we don't, there is no point seeing if we should add nothing
                                  {
                                    buf.insert(buf.begin(), sps_pps.begin(), sps_pps.end());
                                  }
                                  buf.insert(buf.end(), H264_START_SEQUENCE, H264_START_SEQUENCE + sizeof(H264_START_SEQUENCE));
                                  buf.insert(buf.end(), ptr, ptr + nal_size);
                                  const uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                                  double elapsed;
                                  if (prev_video_timestamp)
                                  {
                                    elapsed = static_cast<double>(timestamp - prev_video_timestamp) / 1000.0;
                                  }
                                  else
                                  {
                                    elapsed = 0.0;
                                  }
                                  prev_video_timestamp = timestamp;
                                  video_sr_reporter_->rtpConfig->timestamp += video_sr_reporter_->rtpConfig->secondsToTimestamp(elapsed);
                                  const uint32_t reported_elapsed = video_sr_reporter_->rtpConfig->timestamp - video_sr_reporter_->lastReportedTimestamp();
                                  if (video_sr_reporter_->rtpConfig->timestampToSeconds(reported_elapsed) > 1.0) // RTCP report every second
                                  {
                                    video_sr_reporter_->setNeedsToReport();
                                  }
                                  rtc_video_track_->send(reinterpret_cast<const std::byte*>(buf.data()), buf.size());
                                  ptr += nal_size;
                                  size -= nal_size;
                                }
                              }
                              // Delay loop
                              std::this_thread::sleep_for(std::chrono::milliseconds(10));
                            }
                          });
  }

  const std::string filename_;
  std::shared_ptr<rtc::PeerConnection> peer_connection_;
  std::shared_ptr<rtc::Track> rtc_video_track_;
  std::shared_ptr<rtc::RtcpSrReporter> video_sr_reporter_;
  std::thread thread_;

};

std::vector<char> ReadFile(const std::string& filename)
{
  FILE* file = fopen(filename.c_str(), "rb");
  if (file == nullptr)
  {
    return std::vector<char>();
  }
  setbuf(file, nullptr);

  std::vector<char> result;
  const size_t buf_increase = 128 * 1024;
  while (true)
  {
    const size_t end = result.size();
    result.resize(result.size() + buf_increase, 0);
    const size_t len = fread(result.data() + end, 1, buf_increase * sizeof(char), file) / sizeof(char);
    result.resize(end + len);
    if (ferror(file))
    {
      fclose(file);
      return std::vector<char>();
    }
    if (len < buf_increase)
    {
      break;
    }
  }
  fclose(file);
  return result;
}

int main(int argc, char** argv)
{
  // Args
  if (argc < 2)
  {
#ifdef _WIN32
    std::cout << "./WebRTCPlayer.exe test.mp4" << std::endl;
#else
    std::cout << "./WebRTCPlayer test.mp4" << std::endl;
#endif
    return -1;
  }
  const std::string filename = argv[1];
#ifdef _WIN32
  if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE))
  {
    std::cout << "Failed to register console control handler" << std::endl;
  }
#endif
#ifdef __linux__
  // Signals
  struct sigaction sa;
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = sig;
  sa.sa_flags = SA_RESTART;
  if (sigaction(SIGINT, &sa, nullptr))
  {
    std::cout << "Failed to register SIGINT" << std::endl;
  }
  if (sigaction(SIGTERM, &sa, nullptr))
  {
    std::cout << "Failed to register SIGTERM" << std::endl;
  }
#endif
  // Setup HTTP server
  std::shared_ptr<httplib::Server> http_server = std::make_shared<httplib::Server>();
  http_server->set_tcp_nodelay(true);
  http_server->Get("/", [](const httplib::Request&, httplib::Response& res)
                        {
                          const std::vector<char> index = ReadFile("index.html");
                          res.set_content(index.data(), index.size(), "text/html");
                        });
  std::vector<std::shared_ptr<Connection>> connections;
  http_server->Post("/call", [filename, &connections](const httplib::Request& req, httplib::Response& res)
                             {
                               // Parse JSON request
                               const nlohmann::json body = nlohmann::json::parse(req.body);
                               auto sdp = body.find("sdp");
                               if (sdp == body.end())
                               {
                                 return false;
                               }
                               // Create peer connection
                               std::shared_ptr<Connection> connection = std::make_shared<Connection>(filename);
                               std::promise<std::string> promise;
                               connection->peer_connection_->onGatheringStateChange([connection, &promise](rtc::PeerConnection::GatheringState state)
                                                                                    {
                                                                                      if (state == rtc::PeerConnection::GatheringState::Complete)
                                                                                      {
                                                                                        std::optional<rtc::Description> local_description = connection->peer_connection_->localDescription();
                                                                                        promise.set_value(local_description->generateSdp());
                                                                                      }
                                                                                    });
                               connection->peer_connection_->onStateChange([](rtc::PeerConnection::State state)
                                                                           {
                                                                             if ((state == rtc::PeerConnection::State::Disconnected) || (state == rtc::PeerConnection::State::Failed) || (state == rtc::PeerConnection::State::Closed))
                                                                             {
                                                                               // Ignore
                                                                             }
                                                                           });
                               // Parse SDP request
                               rtc::Description remote_description = rtc::Description(sdp->get<std::string>(), rtc::Description::Type::Offer);
                               for (unsigned int i = 0; i < remote_description.mediaCount(); ++i)
                               {
                                 std::variant<rtc::Description::Media*, rtc::Description::Application*> description = remote_description.media(i);
                                 if (description.index() == 0)
                                 {
                                   rtc::Description::Media* media = std::get<rtc::Description::Media*>(description);
                                   if (media->type() == "video")
                                   {
                                     rtc::Description::Video* video = static_cast<rtc::Description::Video*>(media);
                                     for (const int payloadtype : video->payloadTypes())
                                     {
                                       rtc::Description::Media::RtpMap* rtpmap = video->rtpMap(payloadtype);
                                       if (rtpmap == nullptr)
                                       {
                                         continue;
                                       }
                                       if (rtpmap->format == "H264")
                                       {
                                         if (rtpmap->fmtps.empty()) // Must have this
                                         {
                                           continue;
                                         }
                                         // Add the video track
                                         rtc::Description::Video videodescription = rtc::Description::Video(video->mid());
                                         videodescription.addSSRC(1, "video");
                                         videodescription.addH264Codec(payloadtype, rtpmap->fmtps[0]);
                                         connection->rtc_video_track_ = connection->peer_connection_->addTrack(videodescription);
                                         if (connection->rtc_video_track_ == nullptr)
                                         {
                                           return false;
                                         }
                                         std::shared_ptr<rtc::RtpPacketizationConfig> videortpconfig = std::make_shared<rtc::RtpPacketizationConfig>(1, "video", payloadtype, rtc::H264RtpPacketizer::defaultClockRate);
                                         std::shared_ptr<rtc::H264PacketizationHandler> h264handler = std::make_shared<rtc::H264PacketizationHandler>(std::make_shared<rtc::H264RtpPacketizer>(rtc::H264RtpPacketizer::Separator::LongStartSequence, videortpconfig));
                                         connection->video_sr_reporter_ = std::make_shared<rtc::RtcpSrReporter>(videortpconfig);
                                         h264handler->addToChain(connection->video_sr_reporter_);
                                         h264handler->addToChain(std::make_shared<rtc::RtcpNackResponder>());
                                         connection->rtc_video_track_->setMediaHandler(h264handler);
                                         connection->rtc_video_track_->onOpen([connection]()
                                                                            {
                                                                              connection->video_sr_reporter_->rtpConfig->startTimestamp = 0;
                                                                              connection->Start();
                                                                            });
                                         // Process
                                         connection->peer_connection_->setRemoteDescription(remote_description);
                                         connection->peer_connection_->setLocalDescription();
                                         connections.push_back(connection);
                                         // Respond
                                         const std::string sdp_response = promise.get_future().get();
                                         nlohmann::json response;
                                         response["sdp"] = sdp_response;
                                         res.set_content(response.dump(), "application/json");
                                         return true;
                                       }
                                     }
                                   }
                                 }
                               }
                               return false;
                             });
  std::thread thread([http_server]() { http_server->listen("0.0.0.0", 8080); });
  // Provide user information
  std::cout << "http://127.0.0.1:8080" << std::endl;
  // Running
  while (running)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  // Clear up
  http_server->stop();
  thread.join();
  return 0;
}

