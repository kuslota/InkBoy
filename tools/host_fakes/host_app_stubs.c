/* Stand-ins for InkBoy functions that fakes and library modules never define; a test that links the
 * real definition wins over these weak ones. */
__attribute__((weak)) void game_session_wake(void) {}
__attribute__((weak)) void input_notify_from_isr(void) {}
