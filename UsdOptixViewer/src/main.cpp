// USD OptiX Viewer: loads a USD stage, shows its prim hierarchy and
// properties, and ray traces its geometry with OptiX.
//
//   UsdOptixViewer [file.usd]
#include "App.h"

int main(int argc, char** argv)
{
    App app;
    if (!app.init(argc, argv))
        return 1;
    app.run();
    return 0;
}
