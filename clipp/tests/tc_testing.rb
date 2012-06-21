$:.unshift(File.dirname(File.dirname(__FILE__)))
require 'clipp_test'

class TestTesting < CLIPPTestCase
  def test_input_hashes
    clipp(
        input_hashes: [
          {
            "id" => "input_hash",
            "connection" => {
              "pre_transaction_event" => [
                {
                  "which" => 1,
                  "connection_event" => {
                    "local_ip" => "203.194.118.50",
                    "local_port" => 9665,
                    "remote_ip" => "67.23.33.110",
                    "remote_port" => 443
                  }
                }
              ],
              "transaction" => [
                {
                  "event" => [
                    {
                      "which" => 5,
                      "request_event" => {
                        "raw" => "GET /ssldb/ HTTP/1.1",
                        "method" => "GET",
                        "uri" => "/ssldb/",
                        "protocol" => "HTTP/1.1"
                      }
                    }
                  ]
                }
              ],
              "post_transaction_event" => [
                 {
                   "which" => 4
                 }
              ]
            }
          }
        ]
    )
  end
end
