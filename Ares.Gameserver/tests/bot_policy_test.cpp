#include "Headers/Modules/ShooterGame/BotPolicy.h"
#include <cassert>
#include <cmath>
using namespace BotPolicy;
int main() {
 Facts f; assert(Decide(f)==Action::Wait);
 f.Active=true;f.Enemy=true;assert(Decide(f)==Action::Advance);
 f.Intent=Order::Hold;assert(Decide(f)==Action::Hold);
 f.Visible=true;assert(Decide(f)==Action::Engage);
 f.Health=20;assert(Decide(f)==Action::Recover);
 f.Health=100;f.AmmoKnown=true;f.Ammo=0;f.Reserve=50;assert(Decide(f)==Action::Recover);
 f.Visible=false;assert(Decide(f)==Action::Reload);
 f.Reserve=0;assert(Decide(f)==Action::Hold);
 f.Active=false;assert(Decide(f)==Action::Wait);
 assert(!CanFire(false,0,0,1,25));assert(!CanFire(true,100,0,1,25));
 assert(!CanFire(true,0,5,1,25));assert(!CanFire(true,0,0,.1,25));
 assert(!CanFire(true,0,0,1,0));assert(CanFire(true,0,0,1,25));assert(!CanFire(true,0,0,1,-1));
 for(float dt:{.005f,.016f,.033f,.05f}) {
   AimAxis axis;float angle=0;
   for(int i=0;i<1000;++i){float step=axis.Update(dt,90-angle);assert(std::isfinite(step));assert(std::abs(step)<=240*dt+.001);angle+=step;}
   assert(std::abs(angle-90)<.1);
 }
 assert(KeepEngagement(true,false,.1));assert(!KeepEngagement(true,false,.5));assert(!KeepEngagement(false,true,0));
 assert(BurstSeconds(3000)<BurstSeconds(500));
}
