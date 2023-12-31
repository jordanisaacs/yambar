yambar-modules-battery(5)

# NAME
battery - This module reads battery status

# DESCRIPTION

This module reads battery status from _/sys/class/power_supply_ and
uses *udev* to monitor for changes.

Note that it is common (and "normal") for batteries to be in the state
*unknown* under certain conditions.

For example, some have been seen to enter the *unknown* state when
charging and the capacity reaches ~90%. The battery then stays in
*unknown*, rather than *charging*, until it has been fully charged and
enters the state *full*.

This does not happen with all batteries, and other batteries may enter
the state *unknown* under other conditions.

# TAGS

[[ *Name*
:[ *Type*
:< *Description*
|  name
:  string
:  Battery device name
|  manufacturer
:  string
:  Name of the battery manufacturer
|  model
:  string
:  Battery model name
|  state
:  string
:  One of *full*, *not charging*, *charging*, *discharging* or *unknown*
|  capacity
:  range
:  capacity left, in percent
|  estimate
:  string
:  Estimated time left (to empty while discharging, or to full while
   charging), formatted as HH:MM.

# CONFIGURATION

[[ *Name*
:[ *Type*
:[ *Req*
:< *Description*
|  name
:  string
:  yes
:  Battery device name (one of the names in */sys/class/power_supply*)
|  poll-interval
:  int
:  no
:  How often, in milliseconds, to poll for capacity changes
   (default=*60000*). Set to `0` to disable polling (*warning*: many
   batteries do not support asynchronous reporting). Cannot be less
   than 250ms.

# EXAMPLES

```
bar:
  left:
    - battery:
        name: BAT0
        poll-interval: 30000
        content:
          string: {text: "BAT: {capacity}% {estimate}"}
```

# SEE ALSO

*yambar-modules*(5), *yambar-particles*(5), *yambar-tags*(5), *yambar-decorations*(5)

