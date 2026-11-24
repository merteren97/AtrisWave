using System.Net.Http;
using System.Text;
using System.Text.Json;
using AtrisWave.Core.Automation;
using Xunit;

namespace AtrisWave.Core.Tests;

public sealed class LocalAutomationServerTests
{
    [Fact]
    public async Task RpcEndpoint_HandlesGetStatusAndUnknownMethodOverLoopback()
    {
        using var server = new LocalAutomationServer(
            (method, parameters) => Task.FromResult<object?>(method switch
            {
                "get_status" => new { connected = true, sample_rate = 48000 },
                "load_preset" => new
                {
                    ok = true,
                    name = parameters is JsonElement p && p.TryGetProperty("name", out JsonElement n) ? n.GetString() : null
                },
                _ => new RpcMethodNotFound(),
            }),
            port: 0);

        server.Start();
        Assert.True(server.IsRunning);
        Assert.True(server.Port > 0);

        try
        {
            using var http = new HttpClient();
            string url = $"http://127.0.0.1:{server.Port}/rpc";

            // 1. Valid get_status RPC
            using var req1 = new StringContent("""{"jsonrpc":"2.0","id":1,"method":"get_status"}""", Encoding.UTF8, "application/json");
            HttpResponseMessage resp1 = await http.PostAsync(url, req1);
            resp1.EnsureSuccessStatusCode();
            string body1 = await resp1.Content.ReadAsStringAsync();

            using (JsonDocument doc = JsonDocument.Parse(body1))
            {
                Assert.Equal(1, doc.RootElement.GetProperty("id").GetInt32());
                JsonElement result = doc.RootElement.GetProperty("result");
                Assert.True(result.GetProperty("connected").GetBoolean());
                Assert.Equal(48000, result.GetProperty("sample_rate").GetInt32());
            }

            // 2. Unknown method -> JSON-RPC -32601
            using var req2 = new StringContent("""{"jsonrpc":"2.0","id":2,"method":"nonexistent_cmd"}""", Encoding.UTF8, "application/json");
            HttpResponseMessage resp2 = await http.PostAsync(url, req2);
            resp2.EnsureSuccessStatusCode();
            string body2 = await resp2.Content.ReadAsStringAsync();

            using (JsonDocument doc = JsonDocument.Parse(body2))
            {
                Assert.Equal(2, doc.RootElement.GetProperty("id").GetInt32());
                Assert.Equal(-32601, doc.RootElement.GetProperty("error").GetProperty("code").GetInt32());
            }

            // 3. Multi-byte UTF-8 Turkish preset name (must read exact Content-Length bytes without hanging)
            using var req3 = new StringContent("""{"jsonrpc":"2.0","id":3,"method":"load_preset","params":{"name":"Yayın Mikrofonu"}}""", Encoding.UTF8, "application/json");
            HttpResponseMessage resp3 = await http.PostAsync(url, req3);
            resp3.EnsureSuccessStatusCode();
            string body3 = await resp3.Content.ReadAsStringAsync();

            using (JsonDocument doc = JsonDocument.Parse(body3))
            {
                Assert.Equal(3, doc.RootElement.GetProperty("id").GetInt32());
                JsonElement result = doc.RootElement.GetProperty("result");
                Assert.True(result.GetProperty("ok").GetBoolean());
                Assert.Equal("Yayın Mikrofonu", result.GetProperty("name").GetString());
            }
        }
        finally
        {
            await server.StopAsync();
            Assert.False(server.IsRunning);
        }
    }
}
