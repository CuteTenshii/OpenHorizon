// Prints the physics specification the engine reads for a car, to check the
// database is being interpreted sensibly.

#include "base/Log.h"
#include "car/CarSpec.h"
#include "data/Database.h"
#include "vfs/Vfs.h"

#include <cstdio>
#include <string>

namespace {

void print(const fh::CarSpec& s)
{
    std::printf("%d %s (%d)  class %d  PI %.3f\n", s.id, s.mediaName.c_str(), s.year, s.classId, s.performanceIndex);
    std::printf("  mass %.0f kg, front %.0f%%, wheelbase %.3f, track %.3f/%.3f, cg %.2f m\n", s.massKg,
        s.frontWeight * 100.0F, s.wheelbase, s.frontTrack, s.rearTrack, s.cgHeight);
    std::printf("  engine %s: redline %.0f, idle %.0f, inertia %.3f, braking %.0f Nm\n", s.engineName.c_str(),
        s.redlineRpm, s.idleRpm, s.engineInertia, s.engineBrakingTorque);
    float peakPower = 0.0F;
    float peakRpm = 0.0F;
    float peakTorque = 0.0F;
    for (float rpm = 500.0F; rpm <= s.redlineRpm; rpm += 50.0F) {
        const float t = s.fullThrottleTorque(rpm);
        const float p = t * rpm * 2.0F * 3.14159265F / 60.0F;
        peakTorque = std::max(peakTorque, t);
        if (p > peakPower) {
            peakPower = p;
            peakRpm = rpm;
        }
    }
    std::printf("  peak torque %.0f Nm (game %.0f), peak power %.0f kW at %.0f rpm (game %.0f kW)\n", peakTorque,
        s.reference.peakTorqueNm, peakPower / 1000.0F, peakRpm, s.reference.peakPowerW / 1000.0F);
    std::printf("  drive %d, final %.2f, gears", static_cast<int>(s.drive), s.finalDrive);
    for (float g : s.gears) {
        std::printf(" %.2f", g);
    }
    std::printf(", reverse %.2f, shift %.2f s, clutch %.0f Nm\n", s.reverseRatio, s.shiftTime, s.clutchMaxTorque);
    std::printf("  tyres %.0f/%.0fR%.0f (r %.3f) and %.0f/%.0fR%.0f (r %.3f), compound %s\n", s.frontTires.widthMm,
        s.frontTires.aspect, s.frontTires.rimDiameterIn, s.frontTires.radius(), s.rearTires.widthMm,
        s.rearTires.aspect, s.rearTires.rimDiameterIn, s.rearTires.radius(), s.compound.name.c_str());
    const auto axle = [](const char* name, const fh::AxleSuspension& a) {
        std::printf("  %s: ride %.3f, spring %.0f N/m, damp %.0f/%.0f, bar %.0f Nm/rad, camber %.1f\n", name,
            a.rideHeight, a.springRate, a.bumpDamping, a.reboundDamping, a.antiRollStiffness, a.staticCamberDeg);
    };
    axle("front", s.front);
    axle("rear", s.rear);
    std::printf("  aero: drag %.1f lbf, downforce %.1f/%.1f lbf at 100 mph\n", s.dragAt100MphLbf,
        s.frontDownforceAt100MphLbf, s.rearDownforceAt100MphLbf);
    std::printf("  game sim: 0-60 %.2f s, 0-100 %.2f s, top %.1f m/s, 60-0 %.1f m, lat %.3f g\n",
        s.reference.timeTo60Mph, s.reference.timeTo100Mph, s.reference.topSpeed, s.reference.brakeDistance60Mph,
        s.reference.lateralG60Mph);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: fh1carspec DISC CAR_ID|MEDIA_NAME|all\n");
        return 2;
    }
    try {
        fh::Vfs vfs;
        vfs.mountDisc(argv[1]);
        const fh::Database db(vfs.readOrThrow("media/db/gamedb.slt"));
        const std::string which = argv[2];
        if (which == "all") {
            int failed = 0;
            for (int id : fh::drivableCarIds(db)) {
                try {
                    print(fh::loadCarSpec(db, id, &vfs));
                } catch (const std::exception& e) {
                    ++failed;
                    fh::logError("car {}: {}", id, e.what());
                }
            }
            return failed == 0 ? 0 : 1;
        }
        int id = std::atoi(which.c_str());
        if (id == 0) {
            id = fh::carIdByMediaName(db, which);
        }
        print(fh::loadCarSpec(db, id, &vfs));
        return 0;
    } catch (const std::exception& e) {
        fh::logError("{}", e.what());
        return 1;
    }
}
