"""Exercise the real Muse HTTP fallback with poisoned DNS and validated TLS routing."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/qdtech/tab5'
CJSON = ROOT / 'managed_components/espressif__cjson/cJSON'


@unittest.skipUnless(shutil.which('c++') and shutil.which('cc') and CJSON.exists(),
                     'Host compilers and cJSON required')
class MuseDnsTests(unittest.TestCase):
    def test_poisoned_dns_tls_identity_cache_limits_and_failed_refresh(self):
        source = (BOARD / 'tab5_muse_inbox.cc').read_text()
        helpers = source[source.index('std::string HttpGetDirect('):
                         source.index('std::string JsonString(')]
        connection = source[source.index('RelayConnection CachedRelayConnection('):
                            source.index('Inbox& Inbox::GetInstance()')]
        radio = (BOARD / 'radio_service.cc').read_text()
        radio = radio[radio.index('#if CONFIG_QDTECH_TAB5_NATIVE_UI\n    const auto relay_connection ='):
                      radio.index('    esp_err_t err = esp_http_client_open(client, 0);')]
        radio = radio.replace('return false;', 'return nullptr;')  # setup harness returns its handle
        harness = r'''
#define CONFIG_QDTECH_TAB5_NATIVE_UI 1
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <cJSON.h>
constexpr size_t kMaxJsonBytes=32768;
constexpr int kTimeoutMs=4000,ESP_OK=0;
struct RelayConnection {std::string url;std::string server_name;};
#define ESP_LOGI(...) ((void)0)
int64_t now_us=1000000;
int64_t esp_timer_get_time() { return now_us; }
namespace tab5_net { std::mutex gate; std::mutex& TransferGate() { return gate; } }
struct Reply {int status=0;std::string body;bool complete=true;int64_t length=-1;};
std::map<std::string,std::deque<Reply>> replies;
std::vector<std::string> requests;
Reply Take(const std::string& url) {
 requests.push_back(url);
 auto& q=replies[url];if(q.empty())return {};
 auto r=q.front();q.pop_front();return r;
}
int direct_live=0;
struct DirectHttp {
 Reply reply;size_t pos=0;
 DirectHttp() {++direct_live;} ~DirectHttp() {--direct_live;}
 void SetTimeout(int) {} void SetHeader(const char*,const char*) {}
 bool Open(const char*,const std::string& url) {reply=Take(url);return reply.status!=0;}
 std::optional<int> GetStatusCode() {return reply.status;}
 size_t GetBodyLength() {return reply.body.size();}
 std::optional<size_t> Read(char* b,size_t size) {
  size_t n=std::min(size,reply.body.size()-pos);std::memcpy(b,reply.body.data()+pos,n);pos+=n;return n;
 }
 void Close() {}
};
struct Network {auto CreateHttp(int) {return std::make_unique<DirectHttp>();}};
struct Board {
 static Board& GetInstance() {static Board b;return b;}
 Network* GetNetwork() {static Network n;return &n;}
};
void esp_crt_bundle_attach() {}
struct esp_http_client_config_t {
 const char* url=nullptr;const char* common_name=nullptr;
 void(*crt_bundle_attach)()=nullptr;
 int timeout_ms=0;bool disable_auto_redirect=false;bool skip_cert_common_name_check=false;
 int buffer_size=0,buffer_size_tx=0,max_redirection_count=0;
};
struct Client {Reply reply;size_t pos=0;std::string host;};
using esp_http_client_handle_t=Client*;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* c) {
 assert(direct_live==0); // release the failed client's locks before trying DNS/TLS again
 if(c->common_name)assert(std::string(c->common_name)=="radio.trycloudflare.com" && c->disable_auto_redirect);
 else assert(!c->disable_auto_redirect);
 assert(c->crt_bundle_attach && !c->skip_cert_common_name_check);
 return new Client{Take(c->url),0,{}};
}
int esp_http_client_set_header(Client* c,const char* key,const char* value) {
 if(std::string(key)=="Host")c->host=value;
 return 0;
}
int esp_http_client_open(Client* c,int) {
 assert(c->host=="radio.trycloudflare.com");return c->reply.status ? 0 : -1;
}
int64_t esp_http_client_fetch_headers(Client* c) {return c->reply.length<0?0:c->reply.length;}
int esp_http_client_get_status_code(Client* c) {return c->reply.status;}
int64_t esp_http_client_get_content_length(Client* c) {return c->reply.length;}
int esp_http_client_read(Client* c,char* b,int size) {
 size_t n=std::min(size_t(size),c->reply.body.size()-c->pos);
 std::memcpy(b,c->reply.body.data()+c->pos,n);c->pos+=n;return int(n);
}
bool esp_http_client_is_complete_data_received(Client* c) {return c->reply.complete;}
int esp_http_client_close(Client*) {return 0;}
int esp_http_client_cleanup(Client* c) {delete c;return 0;}
''' + helpers + connection + r'''
namespace tab5_muse {
using ::RelayConnection;
RelayConnection CachedRelayConnection(const std::string& url) {return ::CachedRelayConnection(url);}
}
void SetUi(const char*,const char*) {}
bool IsNetEaseMusicUrl(const std::string& url) {return url.find("music.126.net/")!=std::string::npos;}
Client* CreateRadioClient(const std::string& url,bool playing_custom_url_) {
''' + radio + r'''
return client;
}
int main() {
 const std::string host="radio.trycloudflare.com";
 const std::string url="https://"+host+"/podcast/private-token?limit=6";
 const std::string dns="https://dns.alidns.com/resolve?name="+host+"&type=A";
 const std::string ip1="https://104.16.230.132/podcast/private-token?limit=6";
 const std::string ip2="https://104.16.231.132/podcast/private-token?limit=6";
 const std::string answer=R"({"Status":0,"Question":{"name":"radio.trycloudflare.com.","type":1},
  "Answer":[{"type":1,"data":"104.16.230.132","TTL":60},
            {"type":1,"data":"104.16.231.132","TTL":600}]})";
 assert(TunnelHost(url)==host);
 for(const char* s:{"http://radio.trycloudflare.com/x","https://trycloudflare.com/x",
   "https://radio.trycloudflare.com.evil/x","https://u@radio.trycloudflare.com/x",
   "https://radio.trycloudflare.com:999/x","https://evil/?a.trycloudflare.com"})
  assert(TunnelHost(s).empty());
 for(const char* s:{"127.0.0.1","10.0.0.1","172.16.0.1","192.168.3.200","169.254.1.1",
   "100.64.0.1","198.18.0.22","224.0.0.1","999.1.1.1","1.2.3","1.2.3.4/x","::1"})
  assert(!PublicIpv4(s));
 auto parsed=ParseRelayDns(host,answer,now_us);
 assert(parsed.count==2 && parsed.expires_us==now_us+60000000);
 auto short_ttl=ParseRelayDns(host,R"({"Status":0,"Question":[{"name":"radio.trycloudflare.com","type":1}],
   "Answer":[{"type":1,"data":"104.16.230.132","TTL":1}]})",now_us);
 assert(short_ttl.count==1 && short_ttl.expires_us==now_us+30000000);
 assert(ParseRelayDns(host,R"({"Status":0,"Question":{"name":"radio.trycloudflare.com","type":1},"Answer":[]})",now_us).host.empty());
 assert(ParseRelayDns("wrong.trycloudflare.com",answer,now_us).count==0);
 assert(ParseRelayDns(host,"invalid",now_us).count==0);
 assert(ParseRelayDns(host,R"({"Status":3})",now_us).count==0);
 int status=123;
 // Default DNS fails; the first HTTPS DNS address times out, the second succeeds.
 replies[dns].push_back({200,answer});
 replies[ip2].push_back({200,"{\"episodes\":[{\"id\":9}]}"});
 assert(HttpGet(url,&status)=="{\"episodes\":[{\"id\":9}]}" && status==200);
 assert(requests==std::vector<std::string>({url,dns,ip1,ip2}));
 assert(relay_dns_cache.addresses[0].ip=="104.16.231.132");
 requests.clear();
 // Subsequent polls avoid the poisoned resolver and failed first address.
 replies[ip2].push_back({200,"updated"});
 assert(HttpGet(url,&status)=="updated" && requests==std::vector<std::string>({ip2}));
 // Bounded chunked reads and truncated responses never become valid content.
 replies[ip2].push_back({200,std::string(2000,'x')});
 assert(HttpGet(url,&status,1000).empty() && status==200);
 replies[ip2].push_back({200,"partial",false});
 assert(HttpGet(url,&status).empty() && status==200);
 // A server-side rejection is authoritative; do not bypass it with another address.
 replies[ip2].push_back({409,"conflict"});requests.clear();
 assert(HttpGet(url,&status).empty() && status==409 && requests.size()==1);
 // Expiry returns to the system resolver, which can recover independently.
 now_us+=61000000;replies[url].push_back({200,"normal"});requests.clear();
 assert(HttpGet(url,&status)=="normal" && requests==std::vector<std::string>({url}));
 relay_dns_cache={};requests.clear();
 replies[url].push_back({401,"denied"});
 assert(HttpGet(url,&status).empty() && status==401 && requests.size()==1);
 // DNS error, wrong question and private addresses never receive the token.
 replies[dns].push_back({200,R"({"Status":0,"Question":[{"name":"wrong","type":1}],"Answer":[{"type":1,"data":"127.0.0.1"}]})"});
 requests.clear();assert(HttpGet(url,&status).empty() && status==0);
 assert(requests==std::vector<std::string>({url,dns}));
 assert(direct_live==0);
 // Audio uses the last verified address even while a DNS refresh is in progress.
 const std::string audio="https://"+host+"/audio/private-token/episode.mp3";
 {
  std::lock_guard<std::mutex> transfer(tab5_net::TransferGate());
  auto target=CachedRelayConnection(audio);
  assert(target.url=="https://104.16.231.132/audio/private-token/episode.mp3");
  assert(target.server_name==host); // no TransferGate / network wait on the player task
 }
 requests.clear();
 auto* client=CreateRadioClient(audio,true);
 assert(client && client->host==host);
 assert(requests==std::vector<std::string>({"https://104.16.231.132/audio/private-token/episode.mp3"}));
 esp_http_client_cleanup(client);
 const std::string song="https://cdn.music.126.net/song.mp3";
 auto untouched=CachedRelayConnection(song);
 assert(untouched.url==song && untouched.server_name.empty());
 requests.clear();client=CreateRadioClient(song,true);
 assert(client && client->host.empty() && requests==std::vector<std::string>({song}));
 esp_http_client_cleanup(client);
 now_us+=301000000;
 auto expired=CachedRelayConnection(audio);
 assert(expired.url==audio && expired.server_name.empty());
}
'''
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            cpp, obj, exe = temp/'dns.cc', temp/'cjson.o', temp/'dns'
            cpp.write_text(harness)
            subprocess.run(['cc', '-c', str(CJSON/'cJSON.c'), '-o', str(obj)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(CJSON), str(cpp), str(obj), '-o', str(exe)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
