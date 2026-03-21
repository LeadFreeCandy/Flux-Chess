import { useState } from "react";
import { type WidgetProps, btnStyle } from "./shared";

type Player = "W" | "B";
type Piece = Player | null;
type Position = { x: number; y: number };

// Initial 3x3 Hexapawn board
// Row 0 (Top): Black Pawns
// Row 1 (Mid): Empty
// Row 2 (Bot): White Pawns
const INITIAL_BOARD: Piece[][] = [
  ["B", "B", "B"],
  [null, null, null],
  ["W", "W", "W"],
];

export default function HexapawnWidget({ onStatus }: WidgetProps) {
  const [board, setBoard] = useState<Piece[][]>(INITIAL_BOARD);
  const [turn, setTurn] = useState<Player>("W");
  const [selected, setSelected] = useState<Position | null>(null);
  const [winner, setWinner] = useState<Player | "Draw" | null>(null);
  const [isMoving, setIsMoving] = useState(false);

  const resetGame = async () => {
    setBoard(INITIAL_BOARD);
    setTurn("W");
    setSelected(null);
    setWinner(null);
    // TODO: Have a reset call in here
    await new Promise((resolve) => setTimeout(resolve, 100));

    onStatus("Hexapawn reset. White's turn.");
  };

  const getValidMoves = (player: Player, piecePos: Position, currentBoard: Piece[][]): Position[] => {
    const moves: Position[] = [];
    const dir = player === "W" ? -1 : 1; // White moves UP (-y), Black moves DOWN (+y)
    const forwardY = piecePos.y + dir;

    if (forwardY >= 0 && forwardY <= 2) {
      // Forward move (must be empty)
      if (currentBoard[forwardY][piecePos.x] === null) {
        moves.push({ x: piecePos.x, y: forwardY });
      }
      // Diagonal captures (must contain opponent)
      const opponent = player === "W" ? "B" : "W";
      if (piecePos.x > 0 && currentBoard[forwardY][piecePos.x - 1] === opponent) {
        moves.push({ x: piecePos.x - 1, y: forwardY });
      }
      if (piecePos.x < 2 && currentBoard[forwardY][piecePos.x + 1] === opponent) {
        moves.push({ x: piecePos.x + 1, y: forwardY });
      }
    }
    return moves;
  };

  const executeMove = async (from: Position, to: Position) => {
    setIsMoving(true);
    onStatus(`Moving ${turn} pawn from (${from.x},${from.y}) to (${to.x},${to.y})...`);

    // TODO: Replace this mock delay with actual API calls to the ESP32
    // e.g., await movePieceOnBoard(from, to);
    await new Promise((resolve) => setTimeout(resolve, 100));

    // Update board state
    const newBoard = board.map(row => [...row]);
    newBoard[to.y][to.x] = newBoard[from.y][from.x];
    newBoard[from.y][from.x] = null;
    
    setBoard(newBoard);
    setSelected(null);
    setIsMoving(false);

    // Check Win Conditions
    const nextPlayer = turn === "W" ? "B" : "W";
    
    // Condition 1: Reached the opposite end
    if (to.y === (turn === "W" ? 0 : 2)) {
      setWinner(turn);
      onStatus(`${turn === "W" ? "White" : "Black"} wins by reaching the end!`);
      return;
    }

    // Condition 2: Opponent has no valid moves left or no pieces left
    let opponentHasMoves = false;
    for (let y = 0; y < 3; y++) {
      for (let x = 0; x < 3; x++) {
        if (newBoard[y][x] === nextPlayer) {
          if (getValidMoves(nextPlayer, { x, y }, newBoard).length > 0) {
            opponentHasMoves = true;
          }
        }
      }
    }

    if (!opponentHasMoves) {
      setWinner(turn);
      onStatus(`${turn === "W" ? "White" : "Black"} wins! Opponent has no valid moves.`);
      return;
    }

    setTurn(nextPlayer);
    onStatus(`${nextPlayer === "W" ? "White" : "Black"}'s turn.`);
  };

  const handleSquareClick = (x: number, y: number) => {
    if (winner || isMoving) return;

    const clickedPiece = board[y][x];

    // If we click our own piece, select it
    if (clickedPiece === turn) {
      setSelected({ x, y });
      return;
    }

    // If we have a piece selected, check if this is a valid move
    if (selected) {
      const validMoves = getValidMoves(turn, selected, board);
      const isValid = validMoves.some(m => m.x === x && m.y === y);
      
      if (isValid) {
        executeMove(selected, { x, y });
      } else {
        setSelected(null); // Clicked an invalid square, deselect
      }
    }
  };

  // UI Helpers
  const isSelected = (x: number, y: number) => selected?.x === x && selected?.y === y;
  const isValidTarget = (x: number, y: number) => {
    if (!selected) return false;
    return getValidMoves(turn, selected, board).some(m => m.x === x && m.y === y);
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", height: "100%", alignItems: "center" }}>
      
      {/* Game Status Header */}
      <div style={{ display: "flex", justifyContent: "space-between", width: "100%", maxWidth: 300, marginBottom: 16, alignItems: "center" }}>
        <div style={{ fontSize: 16, fontWeight: "bold", color: winner ? "#4caf50" : "#fff" }}>
          {winner ? `${winner === "W" ? "White" : "Black"} Wins!` : `${turn === "W" ? "White" : "Black"}'s Turn`}
        </div>
        <button onClick={resetGame} disabled={isMoving} style={{ ...btnStyle, background: "#b71c1c", padding: "4px 10px", fontSize: 11 }}>
          Reset Board
        </button>
      </div>

      {/* 3x3 Board */}
      <div style={{ 
        display: "grid", 
        gridTemplateColumns: "repeat(3, 1fr)", 
        gap: 4, 
        background: "#333", 
        padding: 4, 
        borderRadius: 8,
        opacity: isMoving ? 0.6 : 1,
        pointerEvents: isMoving ? "none" : "auto"
      }}>
        {board.map((row, y) => row.map((piece, x) => {
          const isDarkSquare = (x + y) % 2 === 1;
          const bg = isDarkSquare ? "#3a5a7c" : "#D7BA89";
          const highlight = isSelected(x, y) ? "inset 0 0 0 4px #f57f17" : 
                            isValidTarget(x, y) ? "inset 0 0 0 4px #81c784" : "none";
          
          return (
            <div 
              key={`${x}-${y}`}
              onClick={() => handleSquareClick(x, y)}
              style={{
                width: 60,
                height: 60,
                background: bg,
                boxShadow: highlight,
                display: "flex",
                justifyContent: "center",
                alignItems: "center",
                fontSize: 40,
                cursor: "pointer",
                userSelect: "none",
                borderRadius: 2,
                transition: "box-shadow 0.2s"
              }}
            >
              {piece === "W" ? "♙" : piece === "B" ? "♟" : ""}
            </div>
          );
        }))}
      </div>

      {/* Instructions */}
      <div style={{ marginTop: 16, fontSize: 11, color: "#888", textAlign: "center", maxWidth: 300 }}>
        Pawns move straight forward 1 square into empty space, or diagonally 1 square to capture. Win by reaching the other side or blocking your opponent!
      </div>
    </div>
  );
}