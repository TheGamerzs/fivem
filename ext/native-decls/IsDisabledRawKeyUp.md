---
ns: CFX
apiset: client
---
## IS_DISABLED_RAW_KEY_UP

```c
BOOL IS_DISABLED_RAW_KEY_UP(int rawKeyIndex);
```

Gets if the specified `rawKeyIndex` is up, even if the key is disabled with [DISABLE_RAW_KEY_THIS_FRAME](#_0x8BCF0014).

Virtual key codes can be found [here](https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes)

## Parameters
* **rawKeyIndex**: Index of raw key from keyboard.

## Return value
Returns bool value of up state.

## Examples
```lua
if IsDisabledRawKeyUp(32) then -- KEY_SPACE
    print("Spacebar is up")
end
```
