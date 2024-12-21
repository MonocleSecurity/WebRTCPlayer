
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

std::vector<char> ReadFile(const std::string& filename)
{
  FILE* file = fopen(filename.c_str(), "rb");
  if (file == nullptr)
  {
    return std::vector<char>();
  }
  setbuf(file, nullptr);

  std::vector<char> result;
  const size_t bufincrease = 128 * 1024;
  while (true)
  {
    const size_t end = result.size();
    result.resize(result.size() + bufincrease, 0);
    const size_t len = fread(result.data() + end, 1, bufincrease * sizeof(char), file) / sizeof(char);
    result.resize(end + len);
    if (ferror(file))
    {
      fclose(file);
      return std::vector<char>();
    }
    if (len < bufincrease)
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
    std::cout << "./RockchipPlayer test.mp4" << std::endl;
    return -1;
  }
#ifdef _WIN32

  //TODO signal thing please

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
    return -2;//TODO numbers
  }
  if (sigaction(SIGTERM, &sa, nullptr))
  {
    std::cout << "Failed to register SIGTERM" << std::endl;
    return -3;
  }
#endif
  // Open the file
  std::cout << "Opening the file: " << argv[1] << std::endl;
  AVFormatContext* format_context = nullptr;
  if (avformat_open_input(&format_context, argv[1], nullptr, nullptr) != 0)
  {
    std::cout << "Failed to open avformat file: " << argv[1] << std::endl;
    return -4;
  }
  BOOST_SCOPE_EXIT(format_context)
  {
    avformat_close_input(&format_context);
  }
  BOOST_SCOPE_EXIT_END
  if (avformat_find_stream_info(format_context, nullptr) < 0)
  {
    std::cout << "Failed to find stream info: " << argv[1] << std::endl;
    return -5;
  }
  std::optional<unsigned int> videostream;
  for (unsigned int i = 0; i < format_context->nb_streams; i++)
  {
    if ((format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) && (format_context->streams[i]->codecpar->codec_id == AVCodecID::AV_CODEC_ID_H264)) // Currently only support H264
    {
      videostream = i;
      break;
    }
  }
  if (!videostream.has_value())
  {
    std::cout << "Failed to find video stream: " << argv[1] << std::endl;
    return -6;
  }
  // Find SPS/PPS if available and pass it to the decoder
  std::vector<uint8_t> spspps;
  if (format_context->streams[*videostream]->codecpar->extradata && format_context->streams[*videostream]->codecpar->extradata_size)
  {
    const std::vector<uint8_t> extradata(format_context->streams[*videostream]->codecpar->extradata, format_context->streams[*videostream]->codecpar->extradata + format_context->streams[*videostream]->codecpar->extradata_size);
    if (extradata.size())
    {
      if (extradata[0] >= 1) // SPS+PPS count, but we only care about the first one
      {
        const int spscount = extradata[5] & 0x1f;
        const int spsnalsize = (extradata[6] << 8) | extradata[7];
        if ((spsnalsize + 8) <= extradata.size())
        {
          std::cout << "Gathering SPS: " << spsnalsize << std::endl;
          spspps.insert(spspps.end(), extradata.data() + 8, extradata.data() + 8 + spsnalsize);
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
                  spspps.insert(spspps.end(), H264_START_SEQUENCE, H264_START_SEQUENCE + sizeof(H264_START_SEQUENCE));
                  spspps.insert(spspps.end(), extradata.data() + 8 + spsnalsize + 3, extradata.data() + 8 + spsnalsize + 3 + ppsnalsize);
                }
              }
            }
          }
        }
      }
    }
  }
  if (spspps.size())
  {
//TODO?
  }
  // Set libdatachannel components
  
  //TODO probably not needed?

  // Setup HTTP server
  std::shared_ptr<httplib::Server> http_server = std::make_shared<httplib::Server>();
  http_server->set_tcp_nodelay(true);
  http_server->Get("/", [](const httplib::Request&, httplib::Response& res)
                        {
                          const std::vector<char> index = ReadFile("index.html");
                          res.set_content(index.data(), index.size(), "text/html");
                        });
  std::vector<std::shared_ptr<rtc::PeerConnection>> peer_connections;//TODO store nicely
  std::vector<std::shared_ptr<rtc::Track>> rtcvideotracks;//TODO store nicely
  http_server->Post("/call", [&peer_connections, &rtcvideotracks](const httplib::Request& req, httplib::Response& res)
                             {
                               // Parse JSON request
                               const nlohmann::json body = nlohmann::json::parse(req.body);
                               auto id = body.find("id");
                               auto sdp = body.find("sdp");
                               if ((id == body.end()) || (sdp == body.end()))
                               {
                                 return false;
                               }
                               // Create peer connection
                               std::shared_ptr<rtc::PeerConnection> peer_connection = std::make_shared<rtc::PeerConnection>();
                               std::promise<std::string> promise;
                               peer_connection->onGatheringStateChange([peer_connection, &promise](rtc::PeerConnection::GatheringState state)
                                                                       {
                                                                         if (state == rtc::PeerConnection::GatheringState::Complete)
                                                                         {
                                                                           std::optional<rtc::Description> localdescription = peer_connection->localDescription();
                                                                           promise.set_value(localdescription->generateSdp());
                                                                         }
                                                                       });
                               peer_connection->onStateChange([](rtc::PeerConnection::State state)
                                                              {
                                                                if ((state == rtc::PeerConnection::State::Disconnected) || (state == rtc::PeerConnection::State::Failed) || (state == rtc::PeerConnection::State::Closed))
                                                                {
                                                                  int i = 0;//TODO tidy it up? with mutex?
                                                                }
                                                                else
                                                                {
                                                                  int j = 0;//TODO
                                                                }
                                                              });
                               peer_connection->onTrack([](const std::shared_ptr<rtc::Track>& track) {});
                               peer_connection->onIceStateChange([](rtc::PeerConnection::IceState state) {});
                               peer_connection->onSignalingStateChange([](rtc::PeerConnection::SignalingState state) {});
                               // Parse SDP request
                               //TODO probably create some kind of class to hold all this
                               rtc::Description remotedescription = rtc::Description(sdp->get<std::string>(), rtc::Description::Type::Offer);
                               for (unsigned int i = 0; i < remotedescription.mediaCount(); ++i)
                               {
                                 std::variant<rtc::Description::Media*, rtc::Description::Application*> description = remotedescription.media(i);
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
                                         //TODO std::vector<std::string> elements;
                                         //TODO boost::split(elements, rtpmap->fmtps[0], boost::is_any_of(";"));
                                         //TODO // It must be packetization-mode 1, because it supports the packetization model of the library
                                         //TODO std::vector<std::string>::const_iterator packetizationmode = std::find_if(elements.cbegin(), elements.cend(), [](const std::string& element) { return boost::iequals(element, "packetization-mode=1"); }); // Must be explicitly 1 because default is 0
                                         //TODO if (packetizationmode == elements.cend())
                                         //TODO {
                                         //TODO   continue;
                                         //TODO }
                                         //TODO std::vector<std::string>::const_iterator levelasymmetryallowed = std::find_if(elements.cbegin(), elements.cend(), [](const std::string& element) { return boost::iequals(element, "level-asymmetry-allowed=1"); });
                                         //TODO // If must be baseline, main or high profile
                                         //TODO std::vector<std::string>::const_iterator profilelevelid = std::find_if(elements.cbegin(), elements.cend(), [](const std::string& element) { return boost::istarts_with(element, "profile-level-id="); });
                                         //TODO if (profilelevelid == elements.cend())
                                         //TODO {
                                         //TODO   continue;
                                         //TODO }
                                         //TODO std::string type;
                                         //TODO if (boost::istarts_with(profilelevelid->substr(17), "4d"))
                                         //TODO {
                                         //TODO   type = "4d"; // Baseline
                                         //TODO }
                                         //TODO else if (boost::istarts_with(profilelevelid->substr(17), "42"))
                                         //TODO {
                                         //TODO   type = "42"; // Main
                                         //TODO }
                                         //TODO else if (boost::istarts_with(profilelevelid->substr(17), "64"))
                                         //TODO {
                                         //TODO   type = "64"; // High
                                         //TODO }
                                         //TODO else
                                         //TODO {
                                         //TODO   continue;
                                         //TODO }



                                         // Add the video track
                                         rtc::Description::Video videodescription = rtc::Description::Video(video->mid());
                                         videodescription.addSSRC(1, "video");
                                         videodescription.addH264Codec(payloadtype, rtpmap->fmtps[0]);
                                         std::shared_ptr<rtc::Track> rtcvideotrack = peer_connection->addTrack(videodescription);
                                         if (rtcvideotrack == nullptr)
                                         {
                                           return false;
                                         }
                                         std::shared_ptr<rtc::RtpPacketizationConfig> videortpconfig = std::make_shared<rtc::RtpPacketizationConfig>(1, "video", payloadtype, rtc::H264RtpPacketizer::defaultClockRate);
                                         std::shared_ptr<rtc::H264PacketizationHandler> h264handler = std::make_shared<rtc::H264PacketizationHandler>(std::make_shared<rtc::H264RtpPacketizer>(rtc::H264RtpPacketizer::Separator::LongStartSequence, videortpconfig));
                                         std::shared_ptr<rtc::RtcpSrReporter> videosrreporter = std::make_shared<rtc::RtcpSrReporter>(videortpconfig);
                                         h264handler->addToChain(videosrreporter);
                                         h264handler->addToChain(std::make_shared<rtc::RtcpNackResponder>());
                                         rtcvideotrack->setMediaHandler(h264handler);
                                         rtcvideotrack->onOpen([]() {
                                           int i = 0;//TODO         
                                                                      //TODO if (std::shared_ptr<WebRTCClient> c = wc.lock())
                                                                      //TODO {
                                                                      //TODO   c->strand_.post([wc]()
                                                                      //TODO     {
                                                                      //TODO       if (std::shared_ptr<WebRTCClient> c = wc.lock())
                                                                      //TODO       {
                                                                      //TODO         c->SetVideoReady();//TODO inside this we need to start the video reading etc
                                                                      //TODO       }
                                                                      //TODO     });
                                                                      //TODO }
                                                                    });
                                         rtcvideotracks.push_back(rtcvideotrack);
                                         break;
                                       }
                                     }
                                   }
                                 }
                               }

                               //TODO if we didn't find anything, error here

                               // Process SDP
                               peer_connection->setRemoteDescription(remotedescription);
                               peer_connection->setLocalDescription();
                               // Respond
                               const std::string sdp_response = promise.get_future().get();
                               nlohmann::json response;
                               response["sdp"] = sdp_response;
                               res.set_content(response.dump(), "application/json");
                               peer_connections.push_back(peer_connection);//TODO
                               return true;
                             });
  std::thread thread([http_server]() { http_server->listen("0.0.0.0", 80); });
  // Provide user information


//TODO output addresses here and links to see this website

  // Start main loop
  std::cout << "Starting main loop" << std::endl;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  static double time_base = static_cast<double>(format_context->streams[*videostream]->time_base.num) / static_cast<double>(format_context->streams[*videostream]->time_base.den);
  AVPacket* av_packet = nullptr;
  BOOST_SCOPE_EXIT(&av_packet)
  {
    if (av_packet)
    {
      av_packet_free(&av_packet);
    }
  }
  BOOST_SCOPE_EXIT_END
  while (running)
  {
    // Calculate time of frame
    if (av_packet)
    {
      const double av_packet_time = static_cast<double>(av_packet->pts) * time_base;
      const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
      if (now >= (start + std::chrono::milliseconds(static_cast<int>(av_packet_time * 1000.0))))
      {
        av_packet_free(&av_packet);
      }
    }
    // Read frame from file
    if (av_packet == nullptr)
    {
      av_packet = av_packet_alloc();
      int ret = av_read_frame(format_context, av_packet);
      if (ret == AVERROR_EOF)
      {
        ret = av_seek_frame(format_context, *videostream, 0, AVSEEK_FLAG_ANY);
        if (ret < 0)
        {
          std::cout << "Failed to seek frame" << std::endl;
          return -26;
        }
        start = std::chrono::steady_clock::now();
      }
      else if (ret)
      {
        std::cout << "Failed to seek frame" << std::endl;
        return -27;
      }
      // Video
      if (av_packet->stream_index != *videostream)
      {
        av_packet_free(&av_packet);
        continue;
      }
      // Send
      const uint8_t* ptr = av_packet->data;
      size_t size = av_packet->size;
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
		
        //TODO std::cout << nal_size << std::endl;//TODO
//TODO now do something with the frame
		
        ptr += nal_size;
        size -= nal_size;
      }
    }
    // Delay loop
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Clear up
  
//TODO clean up peer connections
  
  return 0;
}

