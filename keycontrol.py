import RPi.GPIO as GPIO
import time

# Define GPIO pins for data, clock, and latch.
PWR_PIN = 1
DATA_PIN = 2
CLOCK_PIN = 3
LATCH_PIN = 4

# How long (in seconds) to light a cell when moving.
MOVE_ON_TIME = 0.001

# Set up GPIO mode and pins.
GPIO.setmode(GPIO.BCM)  # Use BCM numbering; change to GPIO.BOARD if desired.
GPIO.setup(DATA_PIN, GPIO.OUT)
GPIO.setup(CLOCK_PIN, GPIO.OUT)
GPIO.setup(LATCH_PIN, GPIO.OUT)

# Mapping from grid cell (row, col) to shift register output index.
mapping = [
    [3, 4, 1, 0, 5, 2],
    [6, 7, 8, 9, 10, 11],
    [12, 13, 14, 15, 16, 17],
    [18, 19, 20, 21, 22, 23],
    [24, 25, 26, 27, 28, 29],
    [30, 31, 32, 33, 34, 35]
]

def update_shift_registers(bit_list):
    """
    Sends out the entire 40-bit list to the daisy-chained shift registers.
    The list is reversed so that bit_list[39] is shifted out first.
    """
    GPIO.output(LATCH_PIN, GPIO.LOW)
    for bit in reversed(bit_list):
        GPIO.output(CLOCK_PIN, GPIO.LOW)
        GPIO.output(DATA_PIN, GPIO.HIGH if bit else GPIO.LOW)
        GPIO.output(CLOCK_PIN, GPIO.HIGH)
        # Optionally, add a very short delay if needed:
        # time.sleep(0.0001)
    GPIO.output(LATCH_PIN, GPIO.HIGH)

def update_grid(grid):
    """
    Updates the shift registers based on a 6x6 2D boolean array `grid`.
    The 36 grid cells are mapped to shift register bits using `mapping`.
    Unused bits (indices 36-39) are set to False.
    """
    bit_list = [False] * 40
    for row in range(6):
        for col in range(6):
            bit_index = mapping[row][col]
            bit_list[bit_index] = grid[row][col]
    update_shift_registers(bit_list)

def clear_grid():
    """Returns a 6x6 grid with all cells off."""
    return [[False for _ in range(6)] for _ in range(6)]

def print_grid(grid):
    """
    Prints the grid to the console in a simple text format (1 = on, 0 = off).
    """
    for row in grid:
        print(" ".join('1' if cell else '0' for cell in row))
    print()

if __name__ == '__main__':
    # Start with an all-off grid.
    grid = clear_grid()
    update_grid(grid)
    
    # Set the initial position. (You can change this as needed.)
    current_row, current_col = 0, 0

    print("Use WASD to move. (W: Up, A: Left, S: Down, D: Right)")
    print("Press Q to quit.\n")
    
    try:
        while True:
            print(f"Current Position: (Row {current_row}, Col {current_col})")
            # Wait for a command from the user.
            user_input = input("Enter move (W/A/S/D or Q to quit): ").strip().lower()
            
            if user_input == 'q':
                break
            elif user_input not in ['w', 'a', 's', 'd']:
                print("Invalid key. Use W, A, S, D to move or Q to quit.")
                continue
            
            # Calculate new position based on input.
            new_row, new_col = current_row, current_col
            if user_input == 'w':
                new_row -= 1
            elif user_input == 's':
                new_row += 1
            elif user_input == 'a':
                new_col -= 1
            elif user_input == 'd':
                new_col += 1
            
            # Check boundaries (grid is 6x6, rows 0-5, cols 0-5).
            if not (0 <= new_row < 6 and 0 <= new_col < 6):
                print("Move out of bounds. Try a different direction.")
                continue
            
            # Flash the new position:
            grid = clear_grid()  # Start with an all-off grid.
            grid[new_row][new_col] = True
            update_grid(grid)
            time.sleep(MOVE_ON_TIME)
            
            # Clear the flash (turn off the new cell).
            grid = clear_grid()
            update_grid(grid)
            
            # Update current position.
            current_row, current_col = new_row, new_col
            
    finally:
        GPIO.cleanup()
