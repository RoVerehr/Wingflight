Scriptname WingflightAWUCompatibility Hidden
; Original Wingflight helper. No AWU source code is copied or overridden.
; The DLL supplies only validated, player-owned AWU effects.
bool Function SuspendGlide(ActiveMagicEffect wingEffect, bool finishCurrentGlide) global
    if wingEffect == None
        return false
    endif
    wingEffect.UnregisterForControl("Jump")
    wingEffect.UnregisterForUpdate()
    if finishCurrentGlide
        AFW_MagicEffect awuEffect = wingEffect as AFW_MagicEffect
        if awuEffect != None
            ; Let unchanged AWU code release its own glide and restore its own gravity.
            awuEffect.OnControlUp("Jump", 0.0)
        endif
    endif
    return true
EndFunction

bool Function ResumeGlide(ActiveMagicEffect wingEffect, bool awuGlideEnabled) global
    if wingEffect == None
        return false
    endif
    if awuGlideEnabled
        wingEffect.RegisterForControl("Jump")
    else
        wingEffect.UnregisterForControl("Jump")
    endif
    return true
EndFunction
