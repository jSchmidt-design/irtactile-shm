// TelemetryDemoProducer - the C# twin of bindings/cpp/examples/telemetry_demo_producer.
//
// SPDX-License-Identifier: MIT
//
// Publishes core.engine.rpm, core.vehicle.velocity, core.drivetrain.gear, ext.tyre.temp.FL, a local
// channel (demo_wave -> sim.<sourceId>.demo_wave) and an event (demo_bump, every 2 s) at 100 Hz.
// 'a' toggles Active / Inactive, 'q' quits. The source id comes from IRTACTILE_TELEMETRY_SOURCE
// (set by irTactile when it launches the exe as a companion), else "demo".

using IrTactile.Shm.Telemetry;

string name = TelemetrySection.NameFromEnvironment("demo");
using var section = TelemetrySection.Create(name, 256, new ProducerIdentity { SimName = "telemetry_demo_cs" });
TelemetryProducer p = section.Producer;

int rpm = p.Declare(CoreChannels.All[(int)CoreChannel.EngineRpm]);
int vel = p.Declare(CoreChannels.All[(int)CoreChannel.VehicleVelocity]);
int gear = p.Declare(CoreChannels.All[(int)CoreChannel.DrivetrainGear]);
int tyre = p.Declare(ExtChannels.All[(int)ExtChannel.TyreTempFL]);
int wave = p.DeclareLocal("demo_wave", "", -1f, 1f, SignalKind.Continuous);
int bump = p.DeclareLocal("demo_bump", "", 0f, 1f, SignalKind.Event);
p.SetCar("demo_car", "Demo Car");
p.CommitLayout();

Console.WriteLine($"telemetry_demo: section '{name}'\n  a = toggle Active/Inactive, q = quit");

bool active = true;
p.SetState(ProducerState.Active, TelemetryClock.Now);
double freq = TelemetryClock.Frequency;
ulong t0 = TelemetryClock.Now;
ulong nextBump = t0 + (ulong)(2 * freq);
ulong nextReport = t0;
var rv = new TelemetryReturnView();

while (true)
{
    if (Console.KeyAvailable)
    {
        char k = Console.ReadKey(true).KeyChar;
        if (k == 'q') break;
        if (k == 'a')
        {
            active = !active;
            p.SetState(active ? ProducerState.Active : ProducerState.Inactive, TelemetryClock.Now);
            Console.WriteLine($"producerState -> {(active ? "Active" : "Inactive")}");
        }
    }

    ulong now = TelemetryClock.Now;
    double t = (now - t0) / freq;
    if (active)
    {
        p.BeginFrame();
        p.Set(rpm, (float)(3000 + 2500 * Math.Sin(t * 0.8)));
        p.Set(vel, (float)(40 + 30 * Math.Sin(t * 0.3)));
        p.Set(gear, 3f);
        p.Set(tyre, (float)(80 + 10 * Math.Sin(t * 0.1)));
        p.Set(wave, (float)Math.Sin(t * 2 * Math.PI));
        if (now >= nextBump)
        {
            p.Event(bump, 1f);
            nextBump += (ulong)(2 * freq);
        }
        p.EndFrame(now, now);
    }
    else
    {
        p.SetState(ProducerState.Inactive, now);   // heartbeat while idle
    }

    if (now >= nextReport)
    {
        nextReport = now + (ulong)freq;
        if (section.PollReturn(rv))
        {
            int accepted = 0, demanded = 0;
            for (int s = 0; s < p.ChannelCount; s++)
            {
                if (rv.IsAccepted(s)) accepted++;
                if (rv.IsDemanded(s)) demanded++;
            }
            Console.WriteLine($"irTactile: {rv.ConsumerState}, accepted {accepted}/{p.ChannelCount}, demanded {demanded}");
        }
        else
        {
            Console.WriteLine("irTactile: not connected");
        }
    }
    Thread.Sleep(10);
}

p.SetState(ProducerState.Closed, TelemetryClock.Now);
