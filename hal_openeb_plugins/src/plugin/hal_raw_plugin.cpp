/**********************************************************************************************************************
 * Copyright (c) Prophesee S.A.                                                                                       *
 *                                                                                                                    *
 * Licensed under the Apache License, Version 2.0 (the "License");                                                    *
 * you may not use this file except in compliance with the License.                                                   *
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0                                 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is distributed   *
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.                      *
 * See the License for the specific language governing permissions and limitations under the License.                 *
 **********************************************************************************************************************/

#include <string>
#include <memory>
#include <metavision/hal/utils/hal_software_info.h>
#include <metavision/hal/plugin/plugin_entrypoint.h>

#include "common/raw_constants.h"
#include "common/raw_file_discovery.h"

namespace Metavision {
extern "C" {

void initialize_plugin(void *plugin_ptr) {
    Metavision::Plugin &plugin = Metavision::plugin_cast(plugin_ptr);

    plugin.set_plugin_info(Metavision::get_hal_software_info());
    plugin.set_hal_info(Metavision::get_hal_software_info());
    plugin.set_integrator_name(raw_default_integrator);

    plugin.add_file_discovery(std::make_unique<RawFileDiscovery>());
}
}
} // namespace Metavision
