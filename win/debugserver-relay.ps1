# debugserver-relay.ps1
# Non-admin bridge so WSL2 can reach PCSX2's DebugServer, which binds
# 127.0.0.1 only (INADDR_LOOPBACK, see DebugServer.cpp). WSL2 NAT mode cannot
# reach the Windows loopback directly, so this listens on 0.0.0.0 and pumps
# bytes to 127.0.0.1. From WSL connect to the Windows host IP (default gateway,
# e.g. 172.30.x.1) on the same port.
#
# Usage:  powershell -WindowStyle Hidden -ExecutionPolicy Bypass -File debugserver-relay.ps1
# Ports:  21512 DebugServer (breakpoints/registers/memory), 28011 Pine (savestates)
#
# WHY C#: earlier versions pumped bytes with PowerShell scriptblocks on raw
# .NET threads. Any cmdlet call (Write-Host / Add-Content, even New-Object)
# inside such a scriptblock throws PSInvalidOperation
# (ScriptBlock.GetContextFromTLS) which TERMINATES the whole powershell
# process. The relay died on every first client. Real C# threads have no
# runspace context and cannot crash this way. Add-Type compiles with the
# built-in CodeDom provider (csc from .NET Framework) — no admin, no network.

param(
    [int]$ListenPort = 21512,
    [int]$TargetPort = 21512,
    [string]$TargetHost = "127.0.0.1"
)

Add-Type -TypeDefinition @"
using System;
using System.Net;
using System.Net.Sockets;
using System.Threading;

public static class TcpRelay
{
    public static void Run(int listenPort, string targetHost, int targetPort)
    {
        var listener = new TcpListener(IPAddress.Any, listenPort);
        listener.Start();
        Console.WriteLine("[relay] listening 0.0.0.0:" + listenPort + " -> " + targetHost + ":" + targetPort);
        while (true)
        {
            try
            {
                TcpClient client = listener.AcceptTcpClient();
                Console.WriteLine("[relay] client " + client.Client.RemoteEndPoint);
                var upstream = new TcpClient(targetHost, targetPort);
                var cs = client.GetStream();
                var us = upstream.GetStream();
                var done1 = new ManualResetEvent(false);
                var done2 = new ManualResetEvent(false);
                var t1 = new Thread(() => Pump(cs, us, done1)) { IsBackground = true };
                var t2 = new Thread(() => Pump(us, cs, done2)) { IsBackground = true };
                t1.Start();
                t2.Start();
                WaitHandle.WaitAny(new WaitHandle[] { done1, done2 });
                WaitHandle.WaitAny(new WaitHandle[] { done1, done2 });
                client.Close();
                upstream.Close();
            }
            catch (Exception e)
            {
                Console.WriteLine("[relay] error: " + e.Message);
                Thread.Sleep(1000);
            }
        }
    }

    static void Pump(NetworkStream src, NetworkStream dst, ManualResetEvent done)
    {
        try
        {
            byte[] buf = new byte[65536];
            while (true)
            {
                int n = src.Read(buf, 0, buf.Length);
                if (n <= 0) break;
                dst.Write(buf, 0, n);
                dst.Flush();
            }
        }
        catch { }
        finally { done.Set(); }
    }
}
"@

[TcpRelay]::Run($ListenPort, $TargetHost, $TargetPort)
