When calling a function we have 3 possible responses:
- Returnables and Success
- Fail
	- Busy
- No response

## Globals

Board State:
- (Dict of PieceID -> Pos)

Board:
- 2D Array
	- 10x7 (or whatever is correct)
	- 

Bikesheding:
- Stop Moving?
- Temp Values
- Power plug info

## CPP Layer

### Exposed

**Shutdown:**
- No Args, No Return


**Pulse Coil:**
- Request:
	- ARGS:
		- x, y, duration
	- Notes
- Return:
	- Success
	- Error: Invalid Coil/Pulse Too Long/Thermal Limit


**Get board state:**
- Request:
	- ARGS:
		- Right now no args
	- Notes: 
		- Maybe we add
			- Piece certainty (how sure we are that the piece is where we think)
		- We don't have IDs yet, should add
		- Need to work with
			- Missing pieces
			- Too many pieces
			- etc
- Returns:
	- Raw strengths (2D List)
	- Piece Positions (Dict of PieceID -> Pos)


**Move Piece**
- Request
	- ARGS:
		- ID, Ending Pos
- Returns:
	- Pending/Error


**Set Board State:**
- Request:
	- ARGS:
		- New board state
	- Notes:
		- Use Constant RESET if resetting board
- Returns:
	- Pending/Error


SetRGB (underglow)
- Request:
	- ARGS:
		- R, G, B
- Returns:
	- Success
	- PLEASE GOD NO FAILURE
